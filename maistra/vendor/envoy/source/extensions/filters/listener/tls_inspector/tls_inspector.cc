#include "source/extensions/filters/listener/tls_inspector/tls_inspector.h"

#include <cstdint>
#include <string>
#include <vector>

#include "envoy/common/exception.h"
#include "envoy/common/platform.h"
#include "envoy/event/dispatcher.h"
#include "envoy/network/listen_socket.h"
#include "envoy/stats/scope.h"

#include "source/common/api/os_sys_calls_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/hex.h"
#include "source/common/protobuf/utility.h"

#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "openssl/md5.h"
#include "openssl/ssl.h"
#include "ssl/ssl_locl.h"


//#define DISABLE_FINGERPRINTING 1

// [dcillera] TODO: move the following functions to bssl_wrapper
inline bool ssl_client_hello_get_extension(const SSL_CLIENT_HELLO *client_hello,
                                    CBS *out, uint16_t extension_type) {
  CBS extensions;
  CBS_init(&extensions, client_hello->extensions.curr, client_hello->extensions.remaining);
  while (CBS_len(&extensions) != 0) {
    // Decode the next extension.
    uint16_t type;
    CBS extension;
    if (!CBS_get_u16(&extensions, &type) ||
        !CBS_get_u16_length_prefixed(&extensions, &extension)) {
      return false;
    }

    if (type == extension_type) {
      *out = extension;
      return true;
    }
  }

  return false;
}

inline int SSL_early_callback_ctx_extension_get(const SSL_CLIENT_HELLO *client_hello,
                                         uint16_t extension_type,
                                         const uint8_t **out_data,
                                         size_t *out_len) {
  CBS cbs;
  if (!ssl_client_hello_get_extension(client_hello, &cbs, extension_type)) {
    return 0;
  }

  *out_data = CBS_data(&cbs);
  *out_len = CBS_len(&cbs);
  return 1;
}


namespace Envoy {
namespace Extensions {
namespace ListenerFilters {
namespace TlsInspector {

// Min/max TLS version recognized by the underlying TLS/SSL library.
const unsigned Config::TLS_MIN_SUPPORTED_VERSION = TLS1_VERSION;
const unsigned Config::TLS_MAX_SUPPORTED_VERSION = TLS1_3_VERSION;

Config::Config(
    Stats::Scope& scope,
    const envoy::extensions::filters::listener::tls_inspector::v3::TlsInspector& proto_config,
    uint32_t max_client_hello_size)
    : stats_{ALL_TLS_INSPECTOR_STATS(POOL_COUNTER_PREFIX(scope, "tls_inspector."))},
      ssl_ctx_(SSL_CTX_new(TLS_method())),
      enable_ja3_fingerprinting_(
          PROTOBUF_GET_WRAPPED_OR_DEFAULT(proto_config, enable_ja3_fingerprinting, false)),
      max_client_hello_size_(max_client_hello_size) {

  if (max_client_hello_size_ > TLS_MAX_CLIENT_HELLO) {
    throw EnvoyException(fmt::format("max_client_hello_size of {} is greater than maximum of {}.",
                                     max_client_hello_size_, size_t(TLS_MAX_CLIENT_HELLO)));
  }

  SSL_CTX_set_min_proto_version(ssl_ctx_.get(), TLS_MIN_SUPPORTED_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx_.get(), TLS_MAX_SUPPORTED_VERSION);
  SSL_CTX_set_options(ssl_ctx_.get(), SSL_OP_NO_TICKET);
  SSL_CTX_set_session_cache_mode(ssl_ctx_.get(), SSL_SESS_CACHE_OFF);

#ifndef DISABLE_FINGERPRINTING

  SSL_CTX_set_client_hello_cb(
      ssl_ctx_.get(), [](SSL* ssl, int *al, void *arg) -> int {
        Filter* filter = static_cast<Filter*>(SSL_get_app_data(ssl));
        filter->createJA3Hash(ssl->clienthello);

        const uint8_t* data;
        size_t len;
        if (SSL_early_callback_ctx_extension_get(
              ssl->clienthello, TLSEXT_TYPE_application_layer_protocol_negotiation, &data, &len)) {
          filter->onALPN(data, len);
        }
        return SSL_CLIENT_HELLO_SUCCESS;
      },
      nullptr);

#endif  // DISABLE_FINGERPRINTING

  SSL_CTX_set_tlsext_servername_callback(
      ssl_ctx_.get(), +[](SSL* ssl, int* out_alert, void*) -> int {
        Filter* filter = static_cast<Filter*>(SSL_get_app_data(ssl));
        filter->onServername(
            absl::NullSafeStringView(SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)));

        *out_alert = SSL_AD_USER_CANCELLED;
        return SSL_TLSEXT_ERR_OK;
      });

  auto cert_cb = [](SSL* ssl, void*) -> int {
    Filter* filter = static_cast<Filter*>(SSL_get_app_data(ssl));
    // TODO (dmitri-d) move access to SSL internals into bssl_wrapper
    filter->onALPN(ssl->s3->alpn_proposed, ssl->s3->alpn_proposed_len);

    // Return an error to stop the handshake; we have what we wanted already.
    return 0;
  };
  SSL_CTX_set_cert_cb(ssl_ctx_.get(), cert_cb, nullptr);
}

bssl::UniquePtr<SSL> Config::newSsl() { return bssl::UniquePtr<SSL>{SSL_new(ssl_ctx_.get())}; }

thread_local uint8_t Filter::buf_[Config::TLS_MAX_CLIENT_HELLO];

Filter::Filter(const ConfigSharedPtr config) : config_(config), ssl_(config_->newSsl()) {
  RELEASE_ASSERT(sizeof(buf_) >= config_->maxClientHelloSize(), "");

  SSL_set_app_data(ssl_.get(), this);
  SSL_set_accept_state(ssl_.get());
}

Network::FilterStatus Filter::onAccept(Network::ListenerFilterCallbacks& cb) {
  ENVOY_LOG(debug, "tls inspector: new connection accepted");
  Network::ConnectionSocket& socket = cb.socket();
  cb_ = &cb;

  ParseState parse_state = onRead();
  switch (parse_state) {
  case ParseState::Error:
    // As per discussion in https://github.com/envoyproxy/envoy/issues/7864
    // we don't add new enum in FilterStatus so we have to signal the caller
    // the new condition.
    cb.socket().close();
    return Network::FilterStatus::StopIteration;
  case ParseState::Done:
    return Network::FilterStatus::Continue;
  case ParseState::Continue:
    // do nothing but create the event
    socket.ioHandle().initializeFileEvent(
        cb.dispatcher(),
        [this](uint32_t events) {
          if (events & Event::FileReadyType::Closed) {
            config_->stats().connection_closed_.inc();
            done(false);
            return;
          }
          ASSERT(events == Event::FileReadyType::Read);
          ParseState parse_state = onRead();
          switch (parse_state) {
          case ParseState::Error:
            done(false);
            break;
          case ParseState::Done:
            done(true);
            break;
          case ParseState::Continue:
            // do nothing but wait for the next event
            break;
          }
        },
        Event::PlatformDefaultTriggerType, Event::FileReadyType::Read);
    return Network::FilterStatus::StopIteration;
  }

  IS_ENVOY_BUG("unexpected tcp filter parse_state");
  return Network::FilterStatus::StopIteration;
}



void Filter::onALPN(const unsigned char* data, unsigned int len) {
  CBS wire, list;
  CBS_init(&wire, reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(len));
  if (!CBS_get_u16_length_prefixed(&wire, &list) || CBS_len(&wire) != 0 || CBS_len(&list) < 2) {
    // Don't produce errors, let the real TLS stack do it.
    return;
  }
  CBS name;
  std::vector<absl::string_view> protocols;
  while (CBS_len(&list) > 0) {
    if (!CBS_get_u8_length_prefixed(&list, &name) || CBS_len(&name) == 0) {
      // Don't produce errors, let the real TLS stack do it.
      return;
    }
    protocols.emplace_back(reinterpret_cast<const char*>(CBS_data(&name)), CBS_len(&name));
  }
  ENVOY_LOG(trace, "tls:onALPN(), ALPN: {}", absl::StrJoin(protocols, ","));
  cb_->socket().setRequestedApplicationProtocols(protocols);
  alpn_found_ = true;
}

void Filter::onServername(absl::string_view name) {
  if (!name.empty()) {
    config_->stats().sni_found_.inc();
    cb_->socket().setRequestedServerName(name);
    ENVOY_LOG(debug, "tls:onServerName(), requestedServerName: {}", name);
  } else {
    config_->stats().sni_not_found_.inc();
  }
  clienthello_success_ = true;
}

ParseState Filter::onRead() {
  // This receive code is somewhat complicated, because it must be done as a MSG_PEEK because
  // there is no way for a listener-filter to pass payload data to the ConnectionImpl and filters
  // that get created later.
  //
  // We request from the file descriptor to get events every time new data is available,
  // even if previous data has not been read, which is always the case due to MSG_PEEK. When
  // the TlsInspector completes and passes the socket along, a new FileEvent is created for the
  // socket, so that new event is immediately signaled as readable because it is new and the socket
  // is readable, even though no new events have occurred.
  //
  // TODO(ggreenway): write an integration test to ensure the events work as expected on all
  // platforms.
  const auto result = cb_->socket().ioHandle().recv(buf_, config_->maxClientHelloSize(), MSG_PEEK);
  ENVOY_LOG(trace, "tls inspector: recv: {}", result.return_value_);

  if (!result.ok()) {
    if (result.err_->getErrorCode() == Api::IoError::IoErrorCode::Again) {
      return ParseState::Continue;
    }
    config_->stats().read_error_.inc();
    return ParseState::Error;
  }

  if (result.return_value_ == 0) {
    config_->stats().connection_closed_.inc();
    return ParseState::Error;
  }

  // Because we're doing a MSG_PEEK, data we've seen before gets returned every time, so
  // skip over what we've already processed.
  if (static_cast<uint64_t>(result.return_value_) > read_) {
    const uint8_t* data = buf_ + read_;
    const size_t len = result.return_value_ - read_;
    read_ = result.return_value_;
    return parseClientHello(data, len);
  }
  return ParseState::Continue;
}

void Filter::done(bool success) {
  ENVOY_LOG(trace, "tls inspector: done: {}", success);
  cb_->socket().ioHandle().resetFileEvents();
  cb_->continueFilterChain(success);
}

ParseState Filter::parseClientHello(const void* data, size_t len) {
  // Ownership is passed to ssl_ in SSL_set_bio()
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(data, len));

  // Make the mem-BIO return that there is more data
  // available beyond it's end
  BIO_set_mem_eof_return(bio.get(), -1);

  SSL_set_bio(ssl_.get(), bio.get(), bio.get());
  bio.release();

  int ret = SSL_do_handshake(ssl_.get());

  // This should never succeed because an error is always returned from the SNI callback.
  ASSERT(ret <= 0);
  switch (SSL_get_error(ssl_.get(), ret)) {
  case SSL_ERROR_WANT_READ:
    if (read_ == config_->maxClientHelloSize()) {
      // We've hit the specified size limit. This is an unreasonably large ClientHello;
      // indicate failure.
      config_->stats().client_hello_too_large_.inc();
      return ParseState::Error;
    }
    return ParseState::Continue;
  case SSL_ERROR_SSL:
    if (clienthello_success_) {
      config_->stats().tls_found_.inc();
      if (alpn_found_) {
        config_->stats().alpn_found_.inc();
      } else {
        config_->stats().alpn_not_found_.inc();
      }
      cb_->socket().setDetectedTransportProtocol("tls");
    } else {
      config_->stats().tls_not_found_.inc();
    }
    return ParseState::Done;
  default:
    return ParseState::Error;
  }
}

std::vector<absl::string_view> Filter::getAlpnProtocols(const unsigned char* data,
                                                        unsigned int len) {
  std::vector<absl::string_view> protocols;
  if (len == 0) {
    return protocols;
  }

  // The data buffer contains a sequence of non-terminated strings, where each
  // string is preceded by 1 byte that gives it's length (excluding the length
  // byte itself). No string can be zero length, and the last string must not
  // overrun or underrun the buffer.
  unsigned int i = 0;
  do {
    uint8_t protocol_length = data[i++];

    if (protocol_length == 0) {
      ENVOY_LOG_PERIODIC(warn, std::chrono::minutes(1), "tls inspector: ALPN protocol length is zero");
      break;
    }

    if (protocol_length > (len - i)) {
      ENVOY_LOG_PERIODIC(warn, std::chrono::minutes(1), "tls inspector: ALPN protocol length is too long");
      break;
    }

    protocols.emplace_back(reinterpret_cast<const char*>(data + i), protocol_length);
    i += protocol_length;
  } while (i < len);

  return protocols;
}

#ifndef DISABLE_FINGERPRINTING

// Google GREASE values (https://datatracker.ietf.org/doc/html/rfc8701)
static constexpr std::array<uint16_t, 16> GREASE = {
    0x0a0a, 0x1a1a, 0x2a2a, 0x3a3a, 0x4a4a, 0x5a5a, 0x6a6a, 0x7a7a,
    0x8a8a, 0x9a9a, 0xaaaa, 0xbaba, 0xcaca, 0xdada, 0xeaea, 0xfafa,
};

bool isNotGrease(uint16_t id) {
  for (size_t grease_id : GREASE) {
    if (id == grease_id) {
      return false;
    }
  }
  return true;
}

void writeCipherSuites(const SSL_CLIENT_HELLO* ssl_client_hello, std::string& fingerprint) {
  CBS cipher_suites;
  CBS_init(&cipher_suites, ssl_client_hello->ciphersuites.curr, ssl_client_hello->ciphersuites.remaining);

  bool write_cipher = true;
  bool first = true;
  while (write_cipher && CBS_len(&cipher_suites) > 0) {
    uint16_t id;
    write_cipher = CBS_get_u16(&cipher_suites, &id);
    if (write_cipher && isNotGrease(id)) {
      if (!first) {
        absl::StrAppend(&fingerprint, "-");
      }
      absl::StrAppendFormat(&fingerprint, "%d", id);
      first = false;
    }
  }
}

void writeExtensions(const SSL_CLIENT_HELLO* ssl_client_hello, std::string& fingerprint) {
  CBS extensions;
  CBS_init(&extensions, ssl_client_hello->extensions.curr, ssl_client_hello->extensions.remaining);

  bool write_extension = true;
  bool first = true;
  while (write_extension && CBS_len(&extensions) > 0) {
    uint16_t id;
    CBS extension;

    write_extension =
        (CBS_get_u16(&extensions, &id) && CBS_get_u16_length_prefixed(&extensions, &extension));
    if (write_extension && isNotGrease(id)) {
      if (!first) {
        absl::StrAppend(&fingerprint, "-");
      }
      absl::StrAppendFormat(&fingerprint, "%d", id);
      first = false;
    }
  }
}

void writeEllipticCurves(const SSL_CLIENT_HELLO* ssl_client_hello, std::string& fingerprint) {
  const uint8_t* ec_data;
  size_t ec_len;
  if (SSL_early_callback_ctx_extension_get(ssl_client_hello, TLSEXT_TYPE_supported_groups, &ec_data,
                                           &ec_len)) {
    CBS ec;
    CBS_init(&ec, ec_data, ec_len);

    // skip list length
    uint16_t id;
    bool write_elliptic_curve = CBS_get_u16(&ec, &id);

    bool first = true;
    while (write_elliptic_curve && CBS_len(&ec) > 0) {
      write_elliptic_curve = CBS_get_u16(&ec, &id);
      if (write_elliptic_curve) {
        if (!first) {
          absl::StrAppend(&fingerprint, "-");
        }
        absl::StrAppendFormat(&fingerprint, "%d", id);
        first = false;
      }
    }
  }
}

void writeEllipticCurvePointFormats(const SSL_CLIENT_HELLO* ssl_client_hello,
                                    std::string& fingerprint) {
  const uint8_t* ecpf_data;
  size_t ecpf_len;
  if (SSL_early_callback_ctx_extension_get(ssl_client_hello, TLSEXT_TYPE_ec_point_formats,
                                           &ecpf_data, &ecpf_len)) {
    CBS ecpf;
    CBS_init(&ecpf, ecpf_data, ecpf_len);

    // skip list length
    uint8_t id;
    bool write_point_format = CBS_get_u8(&ecpf, &id);

    bool first = true;
    while (write_point_format && CBS_len(&ecpf) > 0) {
      write_point_format = CBS_get_u8(&ecpf, &id);
      if (write_point_format) {
        if (!first) {
          absl::StrAppend(&fingerprint, "-");
        }
        absl::StrAppendFormat(&fingerprint, "%d", id);
        first = false;
      }
    }
  }
}

void Filter::createJA3Hash(const SSL_CLIENT_HELLO* ssl_client_hello) {
  if (config_->enableJA3Fingerprinting()) {
    std::string fingerprint;
    const uint16_t client_version = ssl_client_hello->legacy_version;
    absl::StrAppendFormat(&fingerprint, "%d,", client_version);
    writeCipherSuites(ssl_client_hello, fingerprint);
    absl::StrAppend(&fingerprint, ",");
    writeExtensions(ssl_client_hello, fingerprint);
    absl::StrAppend(&fingerprint, ",");
    writeEllipticCurves(ssl_client_hello, fingerprint);
    absl::StrAppend(&fingerprint, ",");
    writeEllipticCurvePointFormats(ssl_client_hello, fingerprint);

    ENVOY_LOG(trace, "tls:createJA3Hash(), fingerprint: {}", fingerprint);

    uint8_t buf[MD5_DIGEST_LENGTH];
    MD5(reinterpret_cast<const uint8_t*>(fingerprint.data()), fingerprint.size(), buf);
    std::string md5 = Envoy::Hex::encode(buf, MD5_DIGEST_LENGTH);
    ENVOY_LOG(trace, "tls:createJA3Hash(), hash: {}", md5);

    cb_->socket().setJA3Hash(md5);
  }
}

#endif // #ifdef DISABLE_FINGERPRINTING

} // namespace TlsInspector
} // namespace ListenerFilters
} // namespace Extensions
} // namespace Envoy