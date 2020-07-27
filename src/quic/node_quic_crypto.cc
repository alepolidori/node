#include "node_quic_crypto.h"
#include "env-inl.h"
#include "node_crypto.h"
#include "node_crypto_common.h"
#include "node_process.h"
#include "node_quic_session-inl.h"
#include "node_quic_util-inl.h"
#include "node_sockaddr-inl.h"
#include "node_url.h"
#include "string_bytes.h"
#include "v8.h"
#include "util-inl.h"

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <nghttp3/nghttp3.h>  // NGHTTP3_ALPN_H3
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <iterator>
#include <numeric>
#include <unordered_map>
#include <string>
#include <sstream>
#include <vector>

namespace node {

using crypto::EntropySource;
using v8::Local;
using v8::Value;

namespace quic {

bool SessionTicketAppData::Set(const uint8_t* data, size_t len) {
  if (set_)
    return false;
  set_ = true;
  SSL_SESSION_set1_ticket_appdata(session_, data, len);
  return set_;
}

bool SessionTicketAppData::Get(uint8_t** data, size_t* len) const {
  return SSL_SESSION_get0_ticket_appdata(
      session_,
      reinterpret_cast<void**>(data),
      len) == 1;
}

namespace {
constexpr int kCryptoTokenKeylen = 32;
constexpr int kCryptoTokenIvlen = 32;

// Used solely to derive the keys used to generate and
// validate retry tokens. The implementation of this is
// Node.js specific. We use the current implementation
// because it is simple.
bool DeriveTokenKey(
    uint8_t* token_key,
    uint8_t* token_iv,
    const uint8_t* rand_data,
    size_t rand_datalen,
    const ngtcp2_crypto_ctx& ctx,
    const uint8_t* token_secret) {
  static constexpr int kCryptoTokenSecretlen = 32;
  uint8_t secret[kCryptoTokenSecretlen];

  return
      NGTCP2_OK(ngtcp2_crypto_hkdf_extract(
          secret,
          &ctx.md,
          token_secret,
          kTokenSecretLen,
          rand_data,
          rand_datalen)) &&
      NGTCP2_OK(ngtcp2_crypto_derive_packet_protection_key(
          token_key,
          token_iv,
          nullptr,
          &ctx.aead,
          &ctx.md,
          secret,
          kCryptoTokenSecretlen));
}

// Retry tokens are generated only by QUIC servers. They
// are opaque to QUIC clients and must not be guessable by
// on- or off-path attackers. A QUIC server sends a RETRY
// token as a way of initiating explicit path validation
// with a client in response to an initial QUIC packet.
// The client, upon receiving a RETRY, must abandon the
// initial connection attempt and try again, including the
// received retry token in the new initial packet sent to
// the server. If the server is performing explicit
// valiation, it will look for the presence of the retry
// token and validate it if found. The internal structure
// of the retry token must be meaningful to the server,
// and the server must be able to validate the token without
// relying on any state left over from the previous connection
// attempt. The implementation here is entirely Node.js
// specific.
//
// The token is generated by:
// 1. Appending the raw bytes of given socket address, the current
//    timestamp, and the original CID together into a single byte
//    array.
// 2. Generating a block of random data that is used together with
//    the token secret to cryptographically derive an encryption key.
// 3. Encrypting the byte array from step 1 using the encryption key
//    from step 2.
// 4. Appending random data generated in step 2 to the token.
//
// The token secret must be kept secret on the QUIC server that
// generated the retry. When multiple QUIC servers are used in a
// cluster, it cannot be guaranteed that the same QUIC server
// instance will receive the subsequent new Initial packet. Therefore,
// all QUIC servers in the cluster should either share or be aware
// of the same token secret or a mechanism needs to be implemented
// to ensure that subsequent packets are routed to the same QUIC
// server instance.
//
// A malicious peer could attempt to guess the token secret by
// sending a large number specially crafted RETRY-eliciting packets
// to a server then analyzing the resulting retry tokens. To reduce
// the possibility of such attacks, the current implementation of
// QuicSocket generates the token secret randomly for each instance,
// and the number of RETRY responses sent to a given remote address
// should be limited. Such attacks should be of little actual value
// in most cases.
bool GenerateRetryToken(
    uint8_t* token,
    size_t* tokenlen,
    const SocketAddress& addr,
    const QuicCID& ocid,
    const uint8_t* token_secret) {
  std::array<uint8_t, 4096> plaintext;
  uint8_t rand_data[kTokenRandLen];
  uint8_t token_key[kCryptoTokenKeylen];
  uint8_t token_iv[kCryptoTokenIvlen];

  ngtcp2_crypto_ctx ctx;
  ngtcp2_crypto_ctx_initial(&ctx);
  size_t ivlen = ngtcp2_crypto_packet_protection_ivlen(&ctx.aead);
  uint64_t now = uv_hrtime();

  auto p = std::begin(plaintext);
  p = std::copy_n(addr.raw(), addr.length(), p);
  p = std::copy_n(reinterpret_cast<uint8_t*>(&now), sizeof(uint64_t), p);
  p = std::copy_n(ocid->data, ocid->datalen, p);

  EntropySource(rand_data, kTokenRandLen);

  if (!DeriveTokenKey(
          token_key,
          token_iv,
          rand_data,
          kTokenRandLen,
          ctx,
          token_secret)) {
    return false;
  }

  size_t plaintextlen = std::distance(std::begin(plaintext), p);
  if (NGTCP2_ERR(ngtcp2_crypto_encrypt(
          token,
          &ctx.aead,
          plaintext.data(),
          plaintextlen,
          token_key,
          token_iv,
          ivlen,
          addr.raw(),
          addr.length()))) {
    return false;
  }

  *tokenlen = plaintextlen + ngtcp2_crypto_aead_taglen(&ctx.aead);
  memcpy(token + (*tokenlen), rand_data, kTokenRandLen);
  *tokenlen += kTokenRandLen;
  return true;
}
}  // namespace

// A stateless reset token is used when a QUIC endpoint receives a
// QUIC packet with a short header but the associated connection ID
// cannot be matched to any known QuicSession. In such cases, the
// receiver may choose to send a subtle opaque indication to the
// sending peer that state for the QuicSession has apparently been
// lost. For any on- or off- path attacker, a stateless reset packet
// resembles any other QUIC packet with a short header. In order to
// be successfully handled as a stateless reset, the peer must have
// already seen a reset token issued to it associated with the given
// CID. The token itself is opaque to the peer that receives is but
// must be possible to statelessly recreate by the peer that
// originally created it. The actual implementation is Node.js
// specific but we currently defer to a utility function provided
// by ngtcp2.
bool GenerateResetToken(
    uint8_t* token,
    const uint8_t* secret,
    const QuicCID& cid) {
  ngtcp2_crypto_ctx ctx;
  ngtcp2_crypto_ctx_initial(&ctx);
  return NGTCP2_OK(ngtcp2_crypto_generate_stateless_reset_token(
      token,
      &ctx.md,
      secret,
      NGTCP2_STATELESS_RESET_TOKENLEN,
      cid.cid()));
}

// Generates a RETRY packet. See the notes for GenerateRetryToken for details.
std::unique_ptr<QuicPacket> GenerateRetryPacket(
    const uint8_t* token_secret,
    const QuicCID& dcid,
    const QuicCID& scid,
    const SocketAddress& local_addr,
    const SocketAddress& remote_addr) {

  uint8_t token[256];
  size_t tokenlen = sizeof(token);

  if (!GenerateRetryToken(token, &tokenlen, remote_addr, dcid, token_secret))
    return {};

  QuicCID cid;
  EntropySource(cid.data(), kScidLen);
  cid.set_length(kScidLen);

  size_t pktlen = tokenlen + (2 * NGTCP2_MAX_CIDLEN) + scid.length() + 8;

  auto packet = QuicPacket::Create("retry", pktlen);
  ssize_t nwrite =
      ngtcp2_crypto_write_retry(
          packet->data(),
          NGTCP2_MAX_PKTLEN_IPV4,
          scid.cid(),
          cid.cid(),
          dcid.cid(),
          token,
          tokenlen);
  if (nwrite <= 0)
    return {};
  packet->set_length(nwrite);
  return packet;
}

// Validates a retry token included in the given header. This will return
// true if the token cannot be validated, false otherwise. A token is
// valid if it can be successfully decrypted using the key derived from
// random data embedded in the token, the structure of the token matches
// that generated by the GenerateRetryToken function, and the token was
// not generated earlier than now - verification_expiration. If validation
// is successful, ocid will be updated to the original connection ID encoded
// in the encrypted token.
bool InvalidRetryToken(
    const ngtcp2_vec& token,
    const SocketAddress& addr,
    QuicCID* ocid,
    const uint8_t* token_secret,
    uint64_t verification_expiration) {

  if (token.len < kTokenRandLen)
    return true;

  ngtcp2_crypto_ctx ctx;
  ngtcp2_crypto_ctx_initial(&ctx);

  size_t ivlen = ngtcp2_crypto_packet_protection_ivlen(&ctx.aead);

  size_t ciphertextlen = token.len - kTokenRandLen;
  const uint8_t* ciphertext = token.base;
  const uint8_t* rand_data = token.base + ciphertextlen;

  uint8_t token_key[kCryptoTokenKeylen];
  uint8_t token_iv[kCryptoTokenIvlen];

  if (!DeriveTokenKey(
          token_key,
          token_iv,
          rand_data,
          kTokenRandLen,
          ctx,
          token_secret)) {
    return true;
  }

  uint8_t plaintext[4096];

  if (NGTCP2_ERR(ngtcp2_crypto_decrypt(
          plaintext,
          &ctx.aead,
          ciphertext,
          ciphertextlen,
          token_key,
          token_iv,
          ivlen,
          addr.raw(),
          addr.length()))) {
    return true;
  }

  size_t plaintextlen = ciphertextlen - ngtcp2_crypto_aead_taglen(&ctx.aead);
  if (plaintextlen < addr.length() + sizeof(uint64_t))
    return true;

  ssize_t cil = plaintextlen - addr.length() - sizeof(uint64_t);
  if ((cil != 0 && (cil < NGTCP2_MIN_CIDLEN || cil > NGTCP2_MAX_CIDLEN)) ||
      memcmp(plaintext, addr.raw(), addr.length()) != 0) {
    return true;
  }

  uint64_t t;
  memcpy(&t, plaintext + addr.length(), sizeof(uint64_t));

  // 10-second window by default, but configurable for each
  // QuicSocket instance with a MIN_RETRYTOKEN_EXPIRATION second
  // minimum and a MAX_RETRYTOKEN_EXPIRATION second maximum.
  if (t + verification_expiration * NGTCP2_SECONDS < uv_hrtime())
    return true;

  ngtcp2_cid_init(
      ocid->cid(),
      plaintext + addr.length() + sizeof(uint64_t),
      cil);

  return false;
}

// Get the ALPN protocol identifier that was negotiated for the session
Local<Value> GetALPNProtocol(const QuicSession& session) {
  QuicCryptoContext* ctx = session.crypto_context();
  Environment* env = session.env();
  std::string alpn = ctx->selected_alpn();
  // This supposed to be `NGHTTP3_ALPN_H3 + 1`
  // Details see https://github.com/nodejs/node/issues/33959
  if (alpn == &NGHTTP3_ALPN_H3[1]) {
    return env->http3_alpn_string();
  } else {
    return ToV8Value(
      env->context(),
      alpn,
      env->isolate()).FromMaybe(Local<Value>());
  }
}

namespace {
int CertCB(SSL* ssl, void* arg) {
  QuicSession* session = static_cast<QuicSession*>(arg);
  return SSL_get_tlsext_status_type(ssl) == TLSEXT_STATUSTYPE_ocsp ?
      session->crypto_context()->OnOCSP() : 1;
}

void Keylog_CB(const SSL* ssl, const char* line) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  session->crypto_context()->Keylog(line);
}

int Client_Hello_CB(
    SSL* ssl,
    int* tls_alert,
    void* arg) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  int ret = session->crypto_context()->OnClientHello();
  switch (ret) {
    case 0:
      return 1;
    case -1:
      return -1;
    default:
      *tls_alert = ret;
      return 0;
  }
}

int AlpnSelection(
    SSL* ssl,
    const unsigned char** out,
    unsigned char* outlen,
    const unsigned char* in,
    unsigned int inlen,
    void* arg) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));

  unsigned char* tmp;

  // The QuicServerSession supports exactly one ALPN identifier. If that does
  // not match any of the ALPN identifiers provided in the client request,
  // then we fail here. Note that this will not fail the TLS handshake, so
  // we have to check later if the ALPN matches the expected identifier or not.
  if (SSL_select_next_proto(
          &tmp,
          outlen,
          reinterpret_cast<const unsigned char*>(session->alpn().c_str()),
          session->alpn().length(),
          in,
          inlen) == OPENSSL_NPN_NO_OVERLAP) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  *out = tmp;
  return SSL_TLSEXT_ERR_OK;
}

int AllowEarlyDataCB(SSL* ssl, void* arg) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  return session->allow_early_data() ? 1 : 0;
}

int TLS_Status_Callback(SSL* ssl, void* arg) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  return session->crypto_context()->OnTLSStatus();
}

int New_Session_Callback(SSL* ssl, SSL_SESSION* session) {
  QuicSession* s = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  return s->set_session(session);
}

int GenerateSessionTicket(SSL* ssl, void* arg) {
  QuicSession* s = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  SessionTicketAppData app_data(SSL_get_session(ssl));
  s->SetSessionTicketAppData(app_data);
  return 1;
}

SSL_TICKET_RETURN DecryptSessionTicket(
    SSL* ssl,
    SSL_SESSION* session,
    const unsigned char* keyname,
    size_t keyname_len,
    SSL_TICKET_STATUS status,
    void* arg) {
  QuicSession* s = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  SessionTicketAppData::Flag flag = SessionTicketAppData::Flag::STATUS_NONE;
  switch (status) {
    default:
      return SSL_TICKET_RETURN_IGNORE;
    case SSL_TICKET_EMPTY:
      // Fall through
    case SSL_TICKET_NO_DECRYPT:
      return SSL_TICKET_RETURN_IGNORE_RENEW;
    case SSL_TICKET_SUCCESS_RENEW:
      flag = SessionTicketAppData::Flag::STATUS_RENEW;
      // Fall through
    case SSL_TICKET_SUCCESS:
      SessionTicketAppData app_data(session);
      switch (s->GetSessionTicketAppData(app_data, flag)) {
        default:
          return SSL_TICKET_RETURN_IGNORE;
        case SessionTicketAppData::Status::TICKET_IGNORE:
          return SSL_TICKET_RETURN_IGNORE;
        case SessionTicketAppData::Status::TICKET_IGNORE_RENEW:
          return SSL_TICKET_RETURN_IGNORE_RENEW;
        case SessionTicketAppData::Status::TICKET_USE:
          return SSL_TICKET_RETURN_USE;
        case SessionTicketAppData::Status::TICKET_USE_RENEW:
          return SSL_TICKET_RETURN_USE_RENEW;
      }
  }
}

int SetEncryptionSecrets(
    SSL* ssl,
    OSSL_ENCRYPTION_LEVEL ossl_level,
    const uint8_t* read_secret,
    const uint8_t* write_secret,
    size_t secret_len) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  return session->crypto_context()->OnSecrets(
      from_ossl_level(ossl_level),
      read_secret,
      write_secret,
      secret_len) ? 1 : 0;
}

int AddHandshakeData(
    SSL* ssl,
    OSSL_ENCRYPTION_LEVEL ossl_level,
    const uint8_t* data,
    size_t len) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  session->crypto_context()->WriteHandshake(
      from_ossl_level(ossl_level),
      data,
      len);
  return 1;
}

int FlushFlight(SSL* ssl) { return 1; }

int SendAlert(
    SSL* ssl,
    enum ssl_encryption_level_t level,
    uint8_t alert) {
  QuicSession* session = static_cast<QuicSession*>(SSL_get_app_data(ssl));
  session->crypto_context()->set_tls_alert(alert);
  return 1;
}

bool SetTransportParams(QuicSession* session, const crypto::SSLPointer& ssl) {
  ngtcp2_transport_params params;
  ngtcp2_conn_get_local_transport_params(session->connection(), &params);
  uint8_t buf[512];
  ssize_t nwrite = ngtcp2_encode_transport_params(
      buf,
      arraysize(buf),
      NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS,
      &params);
  return nwrite >= 0 &&
         SSL_set_quic_transport_params(ssl.get(), buf, nwrite) == 1;
}

SSL_QUIC_METHOD quic_method = SSL_QUIC_METHOD{
  SetEncryptionSecrets,
  AddHandshakeData,
  FlushFlight,
  SendAlert
};

void SetHostname(const crypto::SSLPointer& ssl, const std::string& hostname) {
  // If the hostname is an IP address, use an empty string
  // as the hostname instead.
    X509_VERIFY_PARAM* param = SSL_get0_param(ssl.get());
    X509_VERIFY_PARAM_set_hostflags(param, 0);

  if (UNLIKELY(SocketAddress::is_numeric_host(hostname.c_str()))) {
    SSL_set_tlsext_host_name(ssl.get(), "");
    CHECK_EQ(X509_VERIFY_PARAM_set1_host(param, "", 0), 1);
  } else {
    SSL_set_tlsext_host_name(ssl.get(), hostname.c_str());
    CHECK_EQ(
      X509_VERIFY_PARAM_set1_host(param, hostname.c_str(), hostname.length()),
      1);
  }
}

}  // namespace

void InitializeTLS(QuicSession* session, const crypto::SSLPointer& ssl) {
  QuicCryptoContext* ctx = session->crypto_context();
  Environment* env = session->env();
  QuicState* quic_state = session->quic_state();

  SSL_set_app_data(ssl.get(), session);
  SSL_set_cert_cb(ssl.get(), CertCB,
                  const_cast<void*>(reinterpret_cast<const void*>(session)));
  SSL_set_verify(ssl.get(), SSL_VERIFY_NONE, crypto::VerifyCallback);

  // Enable tracing if the `--trace-tls` command line flag is used.
  if (env->options()->trace_tls) {
    ctx->EnableTrace();
    if (quic_state->warn_trace_tls) {
      quic_state->warn_trace_tls = false;
      ProcessEmitWarning(env,
          "Enabling --trace-tls can expose sensitive data "
          "in the resulting log");
    }
  }

  switch (ctx->side()) {
    case NGTCP2_CRYPTO_SIDE_CLIENT: {
      SSL_set_connect_state(ssl.get());
      crypto::SetALPN(ssl, session->alpn());
      SetHostname(ssl, session->hostname());
      if (ctx->is_option_set(QUICCLIENTSESSION_OPTION_REQUEST_OCSP))
        SSL_set_tlsext_status_type(ssl.get(), TLSEXT_STATUSTYPE_ocsp);
      break;
    }
    case NGTCP2_CRYPTO_SIDE_SERVER: {
      SSL_set_accept_state(ssl.get());
      if (ctx->is_option_set(QUICSERVERSESSION_OPTION_REQUEST_CERT)) {
        int verify_mode = SSL_VERIFY_PEER;
        if (ctx->is_option_set(QUICSERVERSESSION_OPTION_REJECT_UNAUTHORIZED))
          verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_set_verify(ssl.get(), verify_mode, crypto::VerifyCallback);
      }
      break;
    }
    default:
      UNREACHABLE();
  }

  ngtcp2_conn_set_tls_native_handle(session->connection(), ssl.get());
  SetTransportParams(session, ssl);
}

void InitializeSecureContext(
    BaseObjectPtr<crypto::SecureContext> sc,
    bool early_data,
    ngtcp2_crypto_side side) {
  // TODO(@jasnell): Using a static value for this at the moment but
  // we need to determine if a non-static or per-session value is better.
  constexpr static unsigned char session_id_ctx[] = "node.js quic server";
  switch (side) {
    case NGTCP2_CRYPTO_SIDE_SERVER:
      SSL_CTX_set_options(
          **sc,
          (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
          SSL_OP_SINGLE_ECDH_USE |
          SSL_OP_CIPHER_SERVER_PREFERENCE |
          SSL_OP_NO_ANTI_REPLAY);

      SSL_CTX_set_mode(**sc, SSL_MODE_RELEASE_BUFFERS);

      SSL_CTX_set_alpn_select_cb(**sc, AlpnSelection, nullptr);
      SSL_CTX_set_client_hello_cb(**sc, Client_Hello_CB, nullptr);

      SSL_CTX_set_session_ticket_cb(
          **sc,
          GenerateSessionTicket,
          DecryptSessionTicket,
          nullptr);

      if (early_data) {
        SSL_CTX_set_max_early_data(**sc, 0xffffffff);
        SSL_CTX_set_allow_early_data_cb(**sc, AllowEarlyDataCB, nullptr);
      }

      SSL_CTX_set_session_id_context(
          **sc,
          session_id_ctx,
          sizeof(session_id_ctx) - 1);
      break;
    case NGTCP2_CRYPTO_SIDE_CLIENT:
      SSL_CTX_set_session_cache_mode(
          **sc,
          SSL_SESS_CACHE_CLIENT |
          SSL_SESS_CACHE_NO_INTERNAL_STORE);
      SSL_CTX_sess_set_new_cb(**sc, New_Session_Callback);
      break;
    default:
      UNREACHABLE();
  }
  SSL_CTX_set_min_proto_version(**sc, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(**sc, TLS1_3_VERSION);
  SSL_CTX_set_default_verify_paths(**sc);
  SSL_CTX_set_tlsext_status_cb(**sc, TLS_Status_Callback);
  SSL_CTX_set_keylog_callback(**sc, Keylog_CB);
  SSL_CTX_set_tlsext_status_arg(**sc, nullptr);
  SSL_CTX_set_quic_method(**sc, &quic_method);
}

ngtcp2_crypto_level from_ossl_level(OSSL_ENCRYPTION_LEVEL ossl_level) {
  switch (ossl_level) {
  case ssl_encryption_initial:
    return NGTCP2_CRYPTO_LEVEL_INITIAL;
  case ssl_encryption_early_data:
    return NGTCP2_CRYPTO_LEVEL_EARLY;
  case ssl_encryption_handshake:
    return NGTCP2_CRYPTO_LEVEL_HANDSHAKE;
  case ssl_encryption_application:
    return NGTCP2_CRYPTO_LEVEL_APP;
  default:
    UNREACHABLE();
  }
}

const char* crypto_level_name(ngtcp2_crypto_level level) {
  switch (level) {
    case NGTCP2_CRYPTO_LEVEL_INITIAL:
      return "initial";
    case NGTCP2_CRYPTO_LEVEL_EARLY:
      return "early";
    case NGTCP2_CRYPTO_LEVEL_HANDSHAKE:
      return "handshake";
    case NGTCP2_CRYPTO_LEVEL_APP:
      return "app";
    default:
      UNREACHABLE();
  }
}

// When using IPv6, QUIC recommends the use of IPv6 Flow Labels
// as specified in https://tools.ietf.org/html/rfc6437. These
// are used as a means of reliably associating packets exchanged
// as part of a single flow and protecting against certain kinds
// of attacks.
uint32_t GenerateFlowLabel(
    const SocketAddress& local,
    const SocketAddress& remote,
    const QuicCID& cid,
    const uint8_t* secret,
    size_t secretlen) {
  static constexpr size_t kInfoLen =
      (sizeof(sockaddr_in6) * 2) + NGTCP2_MAX_CIDLEN;

  uint32_t label = 0;

  std::array<uint8_t, kInfoLen> plaintext;
  size_t infolen = local.length() + remote.length() + cid.length();
  CHECK_LE(infolen, kInfoLen);

  ngtcp2_crypto_ctx ctx;
  ngtcp2_crypto_ctx_initial(&ctx);

  auto p = std::begin(plaintext);
  p = std::copy_n(local.raw(), local.length(), p);
  p = std::copy_n(remote.raw(), remote.length(), p);
  p = std::copy_n(cid->data, cid->datalen, p);

  ngtcp2_crypto_hkdf_expand(
      reinterpret_cast<uint8_t*>(&label),
      sizeof(label),
      &ctx.md,
      secret,
      secretlen,
      plaintext.data(),
      infolen);

  label &= kLabelMask;
  DCHECK_LE(label, kLabelMask);
  return label;
}

}  // namespace quic
}  // namespace node
