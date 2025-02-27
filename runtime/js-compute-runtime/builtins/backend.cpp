#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <charconv>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// TODO: remove these once the warnings are fixed
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#include "js/experimental/TypedData.h"
#pragma clang diagnostic pop

#include "builtins/backend.h"
#include "builtins/request-response.h"
#include "host_interface/host_call.h"
#include "js-compute-builtins.h"
#include "js/Conversions.h"

enum class Authentication : uint8_t {
  RSA,
};

enum class KeyExchange : uint8_t {
  EECDH,
  RSA,
};

enum class Encryption : uint8_t {
  AES128,
  AES128GCM,
  AES256,
  AES256GCM,
  CHACHA20POLY1305,
  TRIPLE_DES,
};

enum class EncryptionLevel : uint8_t {
  MEDIUM,
  HIGH,
};

enum class MessageDigest : uint8_t {
  SHA1,
  SHA256,
  SHA384,
  AEAD,
};

enum class Protocol : uint8_t {
  SSLv3,
  TLSv1,
  TLSv1_2,
};

class Cipher {
public:
  std::string_view openSSLAlias;
  KeyExchange kx;
  Authentication au;
  Encryption enc;
  MessageDigest mac;
  Protocol protocol;
  EncryptionLevel level;
  uint16_t strength_bits;

  constexpr Cipher(std::string_view openSSLAlias, KeyExchange kx, Authentication au, Encryption enc,
                   MessageDigest mac, Protocol protocol, EncryptionLevel level, int strength_bits)
      : openSSLAlias(openSSLAlias), kx(kx), au(au), enc(enc), mac(mac), protocol(protocol),
        level(level), strength_bits(strength_bits) {}

  // Overload the == operator
  const bool operator==(const Cipher &obj) const {
    return openSSLAlias == obj.openSSLAlias && kx == obj.kx && au == obj.au && enc == obj.enc &&
           mac == obj.mac && protocol == obj.protocol && level == obj.level &&
           strength_bits == obj.strength_bits;
  }
};

/**
 * Class in charge with parsing openSSL expressions to define a list of ciphers.
 */
class OpenSSLCipherConfigurationParser {
private:
  using AliasMap = std::unordered_map<std::string_view, std::vector<Cipher>>;
  AliasMap aliases;
  // This array should stay aligned with the canonical list located at:
  // https://developer.fastly.com/learning/concepts/routing-traffic-to-fastly/#use-a-tls-configuration
  // The mapping is from OpenSSL cipher names as strings to a the cipher represented as a Cipher
  // object
  static constexpr std::array<std::pair<std::string_view, Cipher>, 11> CIPHER{
      std::pair<std::string_view, Cipher>{
          "DES-CBC3-SHA", Cipher(std::string_view("DES-CBC3-SHA"), KeyExchange::RSA,
                                 Authentication::RSA, Encryption::TRIPLE_DES, MessageDigest::SHA1,
                                 Protocol::SSLv3, EncryptionLevel::MEDIUM, 112)},
      {"AES128-SHA", Cipher(std::string_view("AES128-SHA"), KeyExchange::RSA, Authentication::RSA,
                            Encryption::AES128, MessageDigest::SHA1, Protocol::SSLv3,
                            EncryptionLevel::HIGH, 128)},
      {"AES256-SHA", Cipher(std::string_view("AES256-SHA"), KeyExchange::RSA, Authentication::RSA,
                            Encryption::AES256, MessageDigest::SHA1, Protocol::SSLv3,
                            EncryptionLevel::HIGH, 256)},
      {"AES128-GCM-SHA256", Cipher(std::string_view("AES128-GCM-SHA256"), KeyExchange::RSA,
                                   Authentication::RSA, Encryption::AES128GCM, MessageDigest::AEAD,
                                   Protocol::TLSv1_2, EncryptionLevel::HIGH, 128)},
      {"ECDHE-RSA-AES128-SHA", Cipher(std::string_view("ECDHE-RSA-AES128-SHA"), KeyExchange::EECDH,
                                      Authentication::RSA, Encryption::AES128, MessageDigest::SHA1,
                                      Protocol::TLSv1, EncryptionLevel::HIGH, 128)},
      {"ECDHE-RSA-AES256-SHA", Cipher(std::string_view("ECDHE-RSA-AES256-SHA"), KeyExchange::EECDH,
                                      Authentication::RSA, Encryption::AES256, MessageDigest::SHA1,
                                      Protocol::TLSv1, EncryptionLevel::HIGH, 256)},
      {"ECDHE-RSA-AES128-SHA256",
       Cipher(std::string_view("ECDHE-RSA-AES128-SHA256"), KeyExchange::EECDH, Authentication::RSA,
              Encryption::AES128, MessageDigest::SHA256, Protocol::TLSv1_2, EncryptionLevel::HIGH,
              128)},
      {"ECDHE-RSA-AES256-SHA384",
       Cipher(std::string_view("ECDHE-RSA-AES256-SHA384"), KeyExchange::EECDH, Authentication::RSA,
              Encryption::AES256, MessageDigest::SHA384, Protocol::TLSv1_2, EncryptionLevel::HIGH,
              256)},
      {"ECDHE-RSA-AES128-GCM-SHA256",
       Cipher(std::string_view("ECDHE-RSA-AES128-GCM-SHA256"), KeyExchange::EECDH,
              Authentication::RSA, Encryption::AES128GCM, MessageDigest::AEAD, Protocol::TLSv1_2,
              EncryptionLevel::HIGH, 128)},
      {"ECDHE-RSA-AES256-GCM-SHA384",
       Cipher(std::string_view("ECDHE-RSA-AES256-GCM-SHA384"), KeyExchange::EECDH,
              Authentication::RSA, Encryption::AES256GCM, MessageDigest::AEAD, Protocol::TLSv1_2,
              EncryptionLevel::HIGH, 256)},
      {"ECDHE-RSA-CHACHA20-POLY1305",
       Cipher(std::string_view("ECDHE-RSA-CHACHA20-POLY1305"), KeyExchange::EECDH,
              Authentication::RSA, Encryption::CHACHA20POLY1305, MessageDigest::AEAD,
              Protocol::TLSv1_2, EncryptionLevel::HIGH, 256)},
  };

  static constexpr auto SSL_PROTO_TLSv1_2 = "TLSv1.2";
  static constexpr auto SSL_PROTO_TLSv1_0 = "TLSv1.0";
  static constexpr auto SSL_PROTO_SSLv3 = "SSLv3";
  static constexpr auto SSL_PROTO_TLSv1 = "TLSv1";
  static constexpr auto SSL_PROTO_SSLv2 = "SSLv2";

  static constexpr auto SEPARATOR = ":, ";
  /**
   * If ! is used then the ciphers are permanently deleted from the list. The ciphers deleted can
   * never reappear in the list even if they are explicitly stated.
   */
  static constexpr char EXCLUDE = '!';
  /**
   * If - is used then the ciphers are deleted from the list, but some or all of the ciphers can be
   * added again by later options.
   */
  static constexpr char DELETE = '-';
  /**
   * If + is used then the ciphers are moved to the end of the list. This option doesn't add any new
   * ciphers it just moves matching existing ones.
   */
  static constexpr char TO_END = '+';
  /**
   * Lists of cipher suites can be combined in a single cipher string using the + character.
   * This is used as a logical and operation.
   * For example SHA1+DES represents all cipher suites containing the SHA1 and the DES algorithms.
   */
  static constexpr char AND = '+';
  /**
   * 'high' encryption cipher suites. This currently means those with key lengths larger than 128
   * bits, and some cipher suites with 128-bit keys.
   */
  static constexpr auto HIGH = "HIGH";
  /**
   * 'medium' encryption cipher suites, currently some of those using 128 bit encryption::
   */
  static constexpr auto MEDIUM = "MEDIUM";
  /**
   * Cipher suites using RSA key exchange.
   */
  static constexpr auto kRSA = "kRSA";
  /**
   * Cipher suites using RSA authentication.
   */
  static constexpr auto aRSA = "aRSA";
  /**
   * Cipher suites using RSA for key exchange
   * Despite what the docs say, RSA is equivalent to kRSA.
   */
  static constexpr auto RSA = "RSA";
  /**
   * Cipher suites using ephemeral ECDH key agreement, including anonymous cipher suites.
   */
  static constexpr auto kEECDH = "kEECDH";
  /**
   * Cipher suites using ephemeral ECDH key agreement, excluding anonymous cipher suites.
   * Same as "kEECDH:-AECDH"
   */
  static constexpr auto EECDH = "EECDH";
  /**
   * Cipher suitesusing ECDH key exchange, including anonymous, ephemeral and fixed ECDH.
   */
  static constexpr auto ECDH = "ECDH";
  /**
   * Cipher suites using ephemeral ECDH key agreement, including anonymous cipher suites.
   */
  static constexpr auto kECDHE = "kECDHE";
  /**
   * Cipher suites using authenticated ephemeral ECDH key agreement
   */
  static constexpr auto ECDHE = "ECDHE";
  /**
   * Cipher suites using 128 bit AES.
   */
  static constexpr auto AES128 = "AES128";
  /**
   * Cipher suites using 256 bit AES.
   */
  static constexpr auto AES256 = "AES256";
  /**
   * Cipher suites using either 128 or 256 bit AES.
   */
  static constexpr auto AES = "AES";
  /**
   * AES in Galois Counter Mode (GCM): these cipher suites are only supported in TLS v1.2.
   */
  static constexpr auto AESGCM = "AESGCM";
  /**
   * Cipher suites using CHACHA20.
   */
  static constexpr auto CHACHA20 = "CHACHA20";
  /**
   * Cipher suites using triple DES.
   */
  static constexpr auto TRIPLE_DES = "3DES";
  /**
   * Cipher suites using SHA1.
   */
  static constexpr auto SHA1 = "SHA1";
  /**
   * Cipher suites using SHA1.
   */
  static constexpr auto SHA = "SHA";
  /**
   * Cipher suites using SHA256.
   */
  static constexpr auto SHA256 = "SHA256";
  /**
   * Cipher suites using SHA384.
   */
  static constexpr auto SHA384 = "SHA384";
  // The content of the default list is determined at compile time and normally corresponds to
  // ALL:!COMPLEMENTOFDEFAULT:!eNULL.
  static constexpr auto DEFAULT = "DEFAULT";
  // The ciphers included in ALL, but not enabled by default.
  static constexpr auto COMPLEMENTOFDEFAULT = "COMPLEMENTOFDEFAULT";
  static constexpr auto ALL = "ALL";

  void moveToEnd(const AliasMap &aliases, std::vector<Cipher> &ciphers,
                 std::string_view cipher) const {
    this->moveToEnd(ciphers, aliases.at(cipher));
  }

  void moveToEnd(std::vector<Cipher> &ciphers,
                 const std::vector<Cipher> &ciphersToMoveToEnd) const {
    std::stable_partition(ciphers.begin(), ciphers.end(), [&ciphersToMoveToEnd](auto &cipher) {
      return std::find(ciphersToMoveToEnd.begin(), ciphersToMoveToEnd.end(), cipher) ==
             ciphersToMoveToEnd.end();
    });
  }

  void add(const AliasMap &aliases, std::vector<Cipher> &ciphers, std::string_view alias) const {
    auto toAdd = aliases.at(alias);
    ciphers.insert(ciphers.end(), toAdd.begin(), toAdd.end());
  }

  void remove(const AliasMap &aliases, std::vector<Cipher> &ciphers, std::string_view alias) const {
    auto &toRemove = aliases.at(alias);
    ciphers.erase(std::remove_if(ciphers.begin(), ciphers.end(),
                                 [&](auto &x) {
                                   return std::find(toRemove.begin(), toRemove.end(), x) !=
                                          toRemove.end();
                                 }),
                  ciphers.end());
  }

  void strengthSort(std::vector<Cipher> &ciphers) const {
    /*
     * This routine sorts the ciphers with descending strength. The sorting
     * must keep the pre-sorted sequence.
     */
    std::stable_sort(ciphers.begin(), ciphers.end(),
                     [](auto &l, auto &r) { return l.strength_bits > r.strength_bits; });
  }

  /*
   * See
   * https://github.com/openssl/openssl/blob/709651c9022e7be7e69cf8a2f6edf2c8722a6a1e/ssl/ssl_ciph.c#L1455
   */
  void defaultSort(std::vector<Cipher> &ciphers) const {
    auto byStrength = [](auto &l, auto &r) { return l.strength_bits > r.strength_bits; };
    // order all ciphers by strength first
    std::sort(ciphers.begin(), ciphers.end(), byStrength);

    auto it = std::stable_partition(ciphers.begin(), ciphers.end(),
                                    this->byKeyExchange(KeyExchange::EECDH));

    /* AES is our preferred symmetric cipher */
    auto aes = {Encryption::AES128, Encryption::AES128GCM, Encryption::AES256,
                Encryption::AES256GCM};

    /* Now arrange all ciphers by preference: */
    it = std::stable_partition(it, ciphers.end(), this->byEncryption(aes));

    /* Move ciphers without forward secrecy to the end */;
    std::stable_partition(
        it, ciphers.end(),
        [compare = this->byKeyExchange(KeyExchange::RSA)](auto &c) { return !compare(c); });
  }

  std::function<bool(const Cipher &)> byProtocol(Protocol val) const {
    return [val](auto &c) { return c.protocol == val; };
  }

  std::function<bool(const Cipher &)> byKeyExchange(KeyExchange val) const {
    return [val](auto &c) { return c.kx == val; };
  }

  std::function<bool(const Cipher &)> byAuthentication(Authentication val) const {
    return [val](auto &c) { return c.au == val; };
  }

  std::function<bool(const Cipher &)> byEncryption(std::set<Encryption> vals) const {
    return [vals](auto &c) { return vals.find(c.enc) != vals.end(); };
  }

  std::function<bool(const Cipher &)> byEncryption(Encryption val) const {
    return [val](auto &c) { return c.enc == val; };
  }

  std::function<bool(const Cipher &)> byEncryptionLevel(EncryptionLevel val) const {
    return [val](auto &c) { return c.level == val; };
  }

  std::function<bool(const Cipher &)> byMessageDigest(MessageDigest val) const {
    return [val](auto &c) { return c.mac == val; };
  }

  std::vector<std::string_view> split(std::string_view s, std::string_view delimiter) const {
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string_view> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
      token = s.substr(pos_start, pos_end - pos_start);
      pos_start = pos_end + delim_len;
      res.push_back(token);
    }

    res.push_back(s.substr(pos_start));
    return res;
  }

  std::pair<std::string_view, std::string_view> split_on(std::string_view str, char c) const {
    auto ix = str.find(c);
    if (ix == str.npos) {
      return {str, ""};
    }

    auto left = str.substr(0, ix);
    ix++;
    if (ix >= str.size()) {
      return {left, ""};
    }

    return {left, str.substr(ix)};
  }

  std::vector<std::string_view> splitCipherSuiteString(std::string_view string) const {
    std::vector<std::string_view> result;

    while (!string.empty()) {
      auto [line, rest] = this->split_on(string, ':');
      string = rest;

      while (!line.empty()) {
        auto [part, rest] = this->split_on(line, ',');
        line = rest;
        result.push_back(part);
      }
    }

    return result;
  }

  std::vector<Cipher> all;

public:
  OpenSSLCipherConfigurationParser() {
    this->all.reserve(CIPHER.size());
    for (const auto &any : CIPHER) {
      auto &cipher = any.second;
      this->all.push_back(cipher);
      auto cipherAlias = cipher.openSSLAlias;
      auto alias = aliases.find(cipherAlias);
      if (alias != aliases.end()) {
        alias->second.push_back(cipher);
      } else {
        std::vector<Cipher> list;
        list.push_back(cipher);
        aliases.insert({cipherAlias, list});
      }
      aliases.insert({cipher.openSSLAlias, std::vector<Cipher>{cipher}});
    }

    // Note: the descriptions of the aliases within the comments are from
    // https://www.openssl.org/docs/manmaster/man1/openssl-ciphers.html

    // All cipher suites except the eNULL ciphers (which must be explicitly enabled if needed).
    // As of OpenSSL 1.0.0, the ALL cipher suites are sensibly ordered by default.
    this->defaultSort(this->all);
    aliases.insert({ALL, this->all});
    // "High" encryption cipher suites. This currently means those with key lengths larger than 128
    // bits, and some cipher suites with 128-bit keys.
    std::vector<Cipher> high;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(high),
                 this->byEncryptionLevel(EncryptionLevel::HIGH));
    aliases.insert({HIGH, high});
    // "Medium" encryption cipher suites, currently some of those using 128 bit encryption.
    std::vector<Cipher> medium;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(medium),
                 this->byEncryptionLevel(EncryptionLevel::MEDIUM));
    aliases.insert({MEDIUM, medium});

    // Cipher suites using RSA key exchange or authentication. RSA is an alias for kRSA.
    std::vector<Cipher> krsa;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(krsa),
                 this->byKeyExchange(KeyExchange::RSA));
    aliases.insert({kRSA, krsa});
    std::vector<Cipher> arsa;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(arsa),
                 this->byAuthentication(Authentication::RSA));
    aliases.insert({aRSA, arsa});
    aliases.insert({RSA, krsa});

    // Cipher suites using ephemeral ECDH key agreement, including anonymous cipher suites.
    std::vector<Cipher> ecdh;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(ecdh),
                 this->byKeyExchange(KeyExchange::EECDH));
    aliases.insert({kEECDH, ecdh});
    aliases.insert({kECDHE, ecdh});
    aliases.insert({ECDH, ecdh});
    // Cipher suites using authenticated ephemeral ECDH key agreement.
    aliases.insert({EECDH, ecdh});
    aliases.insert({ECDHE, ecdh});

    // Lists cipher suites which are only supported in at least TLS v1.2, TLS v1.0 or SSL v3.0
    // respectively. Note: there are no cipher suites specific to TLS v1.1. Since this is only the
    // minimum version, if, for example, TLSv1.0 is negotiated then both TLSv1.0 and SSLv3.0 cipher
    // suites are available. Note: these cipher strings do not change the negotiated version of SSL
    // or TLS, they only affect the list of available cipher suites.
    std::vector<Cipher> tlsv2;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(tlsv2),
                 this->byProtocol(Protocol::TLSv1_2));
    aliases.insert({SSL_PROTO_TLSv1_2, tlsv2});
    std::vector<Cipher> tlsv1;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(tlsv1),
                 this->byProtocol(Protocol::TLSv1));
    aliases.insert({SSL_PROTO_TLSv1_0, tlsv1});
    aliases.insert({SSL_PROTO_TLSv1, tlsv1});
    std::vector<Cipher> sslv3;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(sslv3),
                 this->byProtocol(Protocol::SSLv3));
    aliases.insert({SSL_PROTO_SSLv3, sslv3});

    // cipher suites using 128 bit AES.
    std::vector<Cipher> aes128;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(aes128),
                 this->byEncryption({Encryption::AES128, Encryption::AES128GCM}));
    aliases.insert({AES128, aes128});
    // cipher suites using 256 bit AES.
    std::vector<Cipher> aes256;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(aes256),
                 this->byEncryption({Encryption::AES256, Encryption::AES256GCM}));
    aliases.insert({AES256, aes256});
    // cipher suites using either 128 or 256 bit AES.
    auto aes(aes128);
    aes.insert(aes.end(), aes256.begin(), aes256.end());
    aliases.insert({AES, aes});

    // AES in Galois Counter Mode (GCM).
    std::vector<Cipher> aesgcm;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(aesgcm),
                 this->byEncryption({Encryption::AES128GCM, Encryption::AES256GCM}));
    aliases.insert({AESGCM, aesgcm});

    // Cipher suites using ChaCha20.
    std::vector<Cipher> chacha20;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(chacha20),
                 this->byEncryption(Encryption::CHACHA20POLY1305));
    aliases.insert({CHACHA20, chacha20});

    // Cipher suites using triple DES.
    std::vector<Cipher> triple_des;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(triple_des),
                 this->byEncryption(Encryption::TRIPLE_DES));
    aliases.insert({TRIPLE_DES, triple_des});

    // Cipher suites using SHA1.
    std::vector<Cipher> sha1;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(sha1),
                 this->byMessageDigest(MessageDigest::SHA1));
    aliases.insert({SHA1, sha1});
    aliases.insert({SHA, sha1});
    // Cipher suites using SHA256.
    std::vector<Cipher> sha256;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(sha256),
                 this->byMessageDigest(MessageDigest::SHA256));
    aliases.insert({SHA256, sha256});
    // Cipher suites using SHA384.
    std::vector<Cipher> sha384;
    std::copy_if(this->all.begin(), this->all.end(), std::back_inserter(sha384),
                 this->byMessageDigest(MessageDigest::SHA384));
    aliases.insert({SHA384, sha384});

    // COMPLEMENTOFDEFAULT:
    // The ciphers included in ALL, but not enabled by default. Currently this includes all RC4 and
    // anonymous ciphers. Note that this rule does not cover eNULL, which is not included by ALL
    // (use COMPLEMENTOFALL if necessary). Note that RC4 based cipher suites are not supported by
    // Fastly and the only supported anonymous ciphers are `ecdh` and `triple_des`.
    auto complementOfDefault(ecdh);
    complementOfDefault.insert(complementOfDefault.end(), triple_des.begin(), triple_des.end());
    aliases.insert({COMPLEMENTOFDEFAULT, complementOfDefault});

    // The content of the default list is determined at compile time and normally corresponds to
    // ALL:!COMPLEMENTOFDEFAULT:!eNULL.
    aliases.insert({DEFAULT, this->parse("ALL:!COMPLEMENTOFDEFAULT:!eNULL")});
  }

  std::vector<Cipher> parse(std::string_view expression) const {
    /**
     * All ciphers by their openssl alias name.
     */
    auto elements = this->splitCipherSuiteString(expression);
    std::vector<Cipher> ciphers;
    std::vector<Cipher> removedCiphers;
    for (auto &element : elements) {

      if (element.rfind(DELETE, 0) == 0) {
        auto alias = element.substr(1);
        if (aliases.find(alias) != aliases.end()) {
          this->remove(aliases, ciphers, alias);
        }
      } else if (element.rfind(EXCLUDE, 0) == 0) {
        auto alias = element.substr(1);
        auto found = aliases.find(alias);
        if (found != aliases.end()) {

          auto toAdd = found.operator->()->second;
          removedCiphers.insert(removedCiphers.end(), toAdd.begin(), toAdd.end());
        }
      } else if (element.rfind(TO_END, 0) == 0) {
        auto alias = element.substr(1);
        if (aliases.find(alias) != aliases.end()) {
          this->moveToEnd(aliases, ciphers, alias);
        }
      } else if ("@STRENGTH" == element) {
        this->strengthSort(ciphers);
        break;
      } else if (aliases.find(element) != aliases.end()) {
        this->add(aliases, ciphers, element);
      } else if (element.find(AND) != std::string::npos) {
        auto intersections = this->split(element, "+\\");
        if (intersections.size() > 0) {
          auto found = aliases.find(intersections[0]);
          if (found != aliases.end()) {
            auto result{found.operator->()->second};
            for (int i = 1; i < intersections.size(); i++) {
              auto alias = aliases.find(intersections[i]);
              if (alias != aliases.end()) {
                // make `result` only contain the aliases from `alias`
                result.erase(std::remove_if(result.begin(), result.end(),
                                            [&](auto x) {
                                              return std::find(alias->second.begin(),
                                                               alias->second.end(),
                                                               x) != alias->second.end();
                                            }),
                             result.end());
              }
            }
            // Add all of `result` onto `ciphers`
            ciphers.insert(ciphers.end(), result.begin(), result.end());
          }
        }
      }
    }
    // Remove all ciphers from `ciphers` which are contained in `removedCiphers`
    ciphers.erase(std::remove_if(ciphers.begin(), ciphers.end(),
                                 [&removedCiphers](auto &c) {
                                   return std::find(removedCiphers.begin(), removedCiphers.end(),
                                                    c) != removedCiphers.end();
                                 }),
                  ciphers.end());
    return ciphers;
  }
};

std::vector<std::string_view> split(std::string_view string, char delimiter) {
  auto start = 0;
  auto end = string.find(delimiter, start);
  std::vector<std::string_view> result;
  while (end != std::string::npos) {
    result.push_back(string.substr(start, end - start));
    start = end + 1;
    end = string.find(delimiter, start);
  }
  result.push_back(string.substr(start));
  return result;
}

bool isValidIP(std::string_view ip) {
  int format = AF_INET;
  if (ip.find(':') != std::string::npos) {
    format = AF_INET6;
  }

  char octets[sizeof(struct in6_addr)];
  if (inet_pton(format, ip.data(), octets) != 1) {
    return false;
  }
  return true;
}

// A "host" is a "hostname" and an optional "port" in the format hostname:port
// A "hostname" is between 1 and 255 octets -- https://www.rfc-editor.org/rfc/rfc1123#page-13
// A "hostname" must start with a letter or digit -- https://www.rfc-editor.org/rfc/rfc1123#page-13
// A "hostname" is made up of "labels" delimited by a dot `.`
// A "label" is between 1 and 63 octets
bool isValidHost(std::string_view host) {
  if (host.length() < 1) {
    return false;
  }
  auto firstCharacter = host.front();
  // check first character is in the regex [a-zA-Z0-9]
  if (!std::isalnum(firstCharacter)) {
    return false;
  }
  // split the hostname from the port
  int pos = host.find_first_of(':');
  std::string_view hostname = host.substr(0, pos);

  // hostnames can not be longer than 253 characters
  // This is because a hostname is represented as a series of labels, and is terminated by a label
  // of length zero. A label consists of a length octet followed by that number of octets
  // representing the name itself. https://www.rfc-editor.org/rfc/rfc1035#section-3.3
  // https://www.rfc-editor.org/rfc/rfc2181#section-11
  if (hostname.length() > 253) {
    return false;
  }

  auto lastCharacter = hostname.back();
  // check last character is in the regex [a-zA-Z0-9]
  if (!std::isalnum(lastCharacter)) {
    return false;
  }

  auto labels = split(hostname, '.');

  for (auto &label : labels) {
    // Each label in a hostname can not be longer than 63 characters
    // https://www.rfc-editor.org/rfc/rfc2181#section-11
    if (label.length() > 63) {
      return false;
    }

    // Each label can only contain the characters in the regex [a-zA-Z0-9\-]
    auto it = std::find_if_not(label.begin(), label.end(), [](auto character) {
      return std::isalnum(character) || character == '-';
    });
    if (it != label.end()) {

      return false;
    }
  }

  // if there is a port - confirm it is all digits and is between 0 and 65536
  // https://www.iana.org/assignments/service-names-port-numbers/service-names-port-numbers.xhtml
  if (pos != std::string::npos) {
    std::string_view port = host.substr(pos + 1);
    if (!std::all_of(port.begin(), port.end(), [](auto c) { return std::isdigit(c); })) {
      return false;
    }
    int value;
    const std::from_chars_result result =
        std::from_chars(port.data(), port.data() + port.size(), value);
    if (result.ec == std::errc::invalid_argument || result.ec == std::errc::result_out_of_range) {
      return false;
    }
    if (value == 0 || value >= 65536) {
      return false;
    }
  }
  return true;
}

OpenSSLCipherConfigurationParser cipher_parser;

namespace builtins {

bool Backend::isCipherSuiteSupportedByFastly(std::string_view cipherSpec) {
  auto ciphers = cipher_parser.parse(cipherSpec);
  return ciphers.size() > 0;
}
JS::Result<mozilla::Ok> Backend::register_dynamic_backend(JSContext *cx, JS::HandleObject backend) {
  MOZ_ASSERT(is_instance(backend));

  JS::RootedString name(cx, JS::GetReservedSlot(backend, Backend::Slots::Name).toString());
  size_t name_len;
  JS::UniqueChars nameChars = encode(cx, name, &name_len);
  std::string_view name_str{nameChars.get(), name_len};

  JS::RootedString target(cx, JS::GetReservedSlot(backend, Backend::Slots::Target).toString());
  size_t target_len;
  JS::UniqueChars targetChars = encode(cx, target, &target_len);
  std::string_view target_str{targetChars.get(), target_len};

  BackendConfig backend_config;

  auto hostOverrideSlot = JS::GetReservedSlot(backend, Backend::Slots::HostOverride);
  if (!hostOverrideSlot.isNullOrUndefined()) {
    JS::RootedString hostOverrideString(cx, hostOverrideSlot.toString());
    size_t hostOverride_len;
    JS::UniqueChars hostOverrideChars = encode(cx, hostOverrideString, &hostOverride_len);
    backend_config.host_override.emplace(std::move(hostOverrideChars), hostOverride_len);
  }

  auto connectTimeoutSlot = JS::GetReservedSlot(backend, Backend::Slots::ConnectTimeout);
  if (!connectTimeoutSlot.isNullOrUndefined()) {
    backend_config.connect_timeout = connectTimeoutSlot.toInt32();
  }

  auto firstByteTimeoutSlot = JS::GetReservedSlot(backend, Backend::Slots::FirstByteTimeout);
  if (!firstByteTimeoutSlot.isNullOrUndefined()) {
    backend_config.first_byte_timeout = firstByteTimeoutSlot.toInt32();
  }

  auto betweenBytesTimeoutSlot = JS::GetReservedSlot(backend, Backend::Slots::BetweenBytesTimeout);
  if (!betweenBytesTimeoutSlot.isNullOrUndefined()) {
    backend_config.between_bytes_timeout = betweenBytesTimeoutSlot.toInt32();
  }

  auto useSslSlot = JS::GetReservedSlot(backend, Backend::Slots::UseSsl);
  if (!useSslSlot.isNullOrUndefined()) {
    backend_config.use_ssl = useSslSlot.toBoolean();
  }

  auto tlsMinVersion = JS::GetReservedSlot(backend, Backend::Slots::TlsMinVersion);
  if (!tlsMinVersion.isNullOrUndefined()) {
    backend_config.ssl_min_version = static_cast<uint8_t>(tlsMinVersion.toInt32());
  }

  auto tlsMaxVersion = JS::GetReservedSlot(backend, Backend::Slots::TlsMaxVersion);
  if (!tlsMaxVersion.isNullOrUndefined()) {
    backend_config.ssl_max_version = static_cast<int8_t>(tlsMaxVersion.toInt32());
  }

  auto certificateHostnameSlot = JS::GetReservedSlot(backend, Backend::Slots::CertificateHostname);
  if (!certificateHostnameSlot.isNullOrUndefined()) {
    JS::RootedString certificateHostnameString(cx, certificateHostnameSlot.toString());
    size_t certificateHostname_len;
    JS::UniqueChars certificateHostnameChars =
        encode(cx, certificateHostnameString, &certificateHostname_len);
    backend_config.cert_hostname.emplace(std::move(certificateHostnameChars),
                                         certificateHostname_len);
  }

  auto caCertificateSlot = JS::GetReservedSlot(backend, Backend::Slots::CaCertificate);
  if (!caCertificateSlot.isNullOrUndefined()) {
    JS::RootedString caCertificateString(cx, caCertificateSlot.toString());
    size_t caCertificate_len;
    JS::UniqueChars caCertificateChars = encode(cx, caCertificateString, &caCertificate_len);
    backend_config.ca_cert.emplace(std::move(caCertificateChars), caCertificate_len);
  }

  auto ciphersSlot = JS::GetReservedSlot(backend, Backend::Slots::Ciphers);
  if (!ciphersSlot.isNullOrUndefined()) {
    JS::RootedString ciphersString(cx, ciphersSlot.toString());
    size_t ciphers_len;
    JS::UniqueChars ciphersChars = encode(cx, ciphersString, &ciphers_len);
    backend_config.ciphers.emplace(std::move(ciphersChars), ciphers_len);
  }

  auto sniHostnameSlot = JS::GetReservedSlot(backend, Backend::Slots::SniHostname);
  if (!sniHostnameSlot.isNullOrUndefined()) {
    JS::RootedString sniHostnameString(cx, sniHostnameSlot.toString());
    size_t sniHostname_len;
    JS::UniqueChars sniHostnameChars = encode(cx, sniHostnameString, &sniHostname_len);
    backend_config.sni_hostname.emplace(std::move(sniHostnameChars), sniHostname_len);
  }

  auto res = HttpReq::register_dynamic_backend(name_str, target_str, backend_config);
  if (auto *err = res.to_err()) {
    HANDLE_ERROR(cx, *err);
    return JS::Result<mozilla::Ok>(JS::Error());
  }
  return mozilla::Ok();
}

JSString *Backend::name(JSContext *cx, JSObject *self) {
  MOZ_ASSERT(is_instance(self));
  return JS::GetReservedSlot(self, Backend::Slots::Name).toString();
}

bool Backend::toString(JSContext *cx, unsigned argc, JS::Value *vp) {
  METHOD_HEADER(0)
  JS::RootedString name(cx, JS::GetReservedSlot(self, Backend::Slots::Name).toString());
  args.rval().setString(name);
  return true;
}

const JSFunctionSpec Backend::static_methods[] = {
    JS_FS_END,
};

const JSPropertySpec Backend::static_properties[] = {
    JS_PS_END,
};

const JSFunctionSpec Backend::methods[] = {JS_FN("toString", toString, 0, JSPROP_ENUMERATE),
                                           JS_FS_END};

const JSPropertySpec Backend::properties[] = {JS_PS_END};

bool Backend::set_name(JSContext *cx, JSObject *backend, JS::HandleValue name_val) {
  if (name_val.isNullOrUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_NAME_NOT_SET);
    return false;
  }
  JS::RootedString name(cx, JS::ToString(cx, name_val));
  if (!name) {
    return false;
  }
  auto length = JS::GetStringLength(name);
  if (length > 254) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_NAME_TOO_LONG);
    return false;
  }
  if (length == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_NAME_EMPTY);
    return false;
  }

  JS::SetReservedSlot(backend, Backend::Slots::Name, JS::StringValue(name));
  return true;
}

bool Backend::set_host_override(JSContext *cx, JSObject *backend,
                                JS::HandleValue hostOverride_val) {
  auto hostOverride = JS::ToString(cx, hostOverride_val);
  if (!hostOverride) {
    return false;
  }

  if (JS_GetStringLength(hostOverride) == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_HOST_OVERRIDE_EMPTY);
    return false;
  }
  JS::SetReservedSlot(backend, Backend::Slots::HostOverride, JS::StringValue(hostOverride));
  return true;
}

bool Backend::set_sni_hostname(JSContext *cx, JSObject *backend, JS::HandleValue sniHostname_val) {
  auto sniHostname = JS::ToString(cx, sniHostname_val);
  if (!sniHostname) {
    return false;
  }

  if (JS_GetStringLength(sniHostname) == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_SNI_HOSTNAME_EMPTY);
    return false;
  }
  JS::SetReservedSlot(backend, Backend::Slots::SniHostname, JS::StringValue(sniHostname));
  return true;
}

/// Timeouts for backends must be less than 2^32 milliseconds, or
/// about a month and a half.
bool Backend::set_timeout_slot(JSContext *cx, JSObject *backend, JS::HandleValue value,
                               Backend::Slots slot, std::string property_name) {
  double native_value;
  if (!JS::ToNumber(cx, value, &native_value)) {
    return false;
  }
  int64_t timeout = std::round(native_value);
  if (timeout < 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TIMEOUT_NEGATIVE,
                              property_name.c_str());
    return false;
  }
  if (timeout >= std::pow(2, 32)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TIMEOUT_TOO_BIG,
                              property_name.c_str());
    return false;
  }
  JS::SetReservedSlot(backend, slot, JS::Int32Value(timeout));
  return true;
}

bool Backend::set_target(JSContext *cx, JSObject *backend, JS::HandleValue target_val) {
  if (target_val.isNullOrUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TARGET_NOT_SET);
    return false;
  }

  auto targetStringSlice = encode(cx, target_val);
  if (!targetStringSlice.data) {
    return false;
  }

  std::string_view targetString(reinterpret_cast<char *>(targetStringSlice.data),
                                targetStringSlice.len);
  auto length = targetString.length();
  if (length == 0) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TARGET_EMPTY);
    return false;
  }

  if (targetString == "::") {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TARGET_INVALID);
    return false;
  }
  if (!isValidHost(targetString) && !isValidIP(targetString)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TARGET_INVALID);
    return false;
  }

  auto targetStr = JS_NewStringCopyN(cx, targetString.data(), targetString.length());
  if (!targetStr) {
    return false;
  }
  JS::RootedValue target(cx, JS::StringValue(targetStr));
  JS::SetReservedSlot(backend, Backend::Slots::Target, target);
  return true;
}

JSObject *Backend::create(JSContext *cx, JS::HandleObject request) {
  JS::RootedValue request_url(cx, RequestOrResponse::url(request));
  auto url_string = encode(cx, request_url);
  if (!url_string.data) {
    return nullptr;
  }

  auto url = jsurl::new_jsurl(&url_string);
  if (!url) {
    JS_ReportErrorUTF8(cx, "URL constructor: %s is not a valid URL.", (char *)url_string.data);
    return nullptr;
  }
  const jsurl::SpecSlice slice = jsurl::host(url);
  auto nameJSStr = JS_NewStringCopyN(cx, (char *)slice.data, slice.len);
  if (!nameJSStr) {
    return nullptr;
  }
  std::string name_str((char *)slice.data, slice.len);

  // Check if we already constructed an implicit dynamic backend for this host.
  bool found;
  JS::RootedValue alreadyBuiltBackend(cx);
  if (!JS_HasProperty(cx, Backend::backends, name_str.c_str(), &found)) {
    return nullptr;
  }
  if (found) {
    if (!JS_GetProperty(cx, Backend::backends, name_str.c_str(), &alreadyBuiltBackend)) {
      return nullptr;
    }
    JS::RootedObject backend(cx, &alreadyBuiltBackend.toObject());
    return backend;
  }

  auto backendInstance = JS_NewObjectWithGivenProto(cx, &Backend::class_, Backend::proto_obj);
  if (!backendInstance) {
    return nullptr;
  }
  JS::RootedValue backendVal(cx, JS::ObjectValue(*backendInstance));
  JS::RootedObject backend(cx, backendInstance);
  if (!backend) {
    return nullptr;
  }

  JS::RootedValue name(cx, JS::StringValue(nameJSStr));
  if (!Backend::set_name(cx, backend, name)) {
    return nullptr;
  }
  if (!Backend::set_host_override(cx, backend, name)) {
    return nullptr;
  }
  if (!Backend::set_target(cx, backend, name)) {
    return nullptr;
  }
  const jsurl::SpecString origin_specstring = jsurl::origin(url);
  std::string_view origin((char *)origin_specstring.data, origin_specstring.len);

  auto use_ssl = origin.rfind("https://", 0) == 0;
  JS::SetReservedSlot(backend, Backend::Slots::UseSsl, JS::BooleanValue(use_ssl));
  if (use_ssl) {
    if (!Backend::set_sni_hostname(cx, backend, name)) {
      return nullptr;
    }
  }

  auto result = Backend::register_dynamic_backend(cx, backend);
  if (result.isErr()) {
    return nullptr;
  } else {
    if (!JS_SetProperty(cx, Backend::backends, name_str.c_str(), backendVal)) {
      return nullptr;
    }
    return backend;
  }
}

bool Backend::constructor(JSContext *cx, unsigned argc, JS::Value *vp) {
  REQUEST_HANDLER_ONLY("The Backend builtin");
  CTOR_HEADER("Backend", 1);

  auto configurationParameter = args.get(0);

  if (!configurationParameter.isObject()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_PARAMETER_NOT_OBJECT);
    return false;
  }

  JS::RootedObject backend(cx, JS_NewObjectForConstructor(cx, &class_, args));
  if (!backend) {
    return false;
  }

  JS::RootedObject configuration(cx, &configurationParameter.toObject());

  JS::RootedValue name_val(cx);
  if (!JS_GetProperty(cx, configuration, "name", &name_val)) {
    return false;
  }
  if (!Backend::set_name(cx, backend, name_val)) {
    return false;
  }

  JS::RootedValue target_val(cx);
  if (!JS_GetProperty(cx, configuration, "target", &target_val)) {
    return false;
  }
  if (!Backend::set_target(cx, backend, target_val)) {
    return false;
  }

  bool found;
  JS::RootedValue hostOverride_val(cx);
  if (!JS_HasProperty(cx, configuration, "hostOverride", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "hostOverride", &hostOverride_val)) {
      return false;
    }
    if (!Backend::set_host_override(cx, backend, hostOverride_val)) {
      return false;
    }
  }

  JS::RootedValue connectTimeout_val(cx);
  if (!JS_HasProperty(cx, configuration, "connectTimeout", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "connectTimeout", &connectTimeout_val)) {
      return false;
    }
    if (!Backend::set_timeout_slot(cx, backend, connectTimeout_val, Backend::Slots::ConnectTimeout,
                                   "connectTimeout")) {
      return false;
    }
  }

  /// Timeouts for backends must be less than 2^32 milliseconds, or
  /// about a month and a half.
  JS::RootedValue firstByteTimeout_val(cx);
  if (!JS_HasProperty(cx, configuration, "firstByteTimeout", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "firstByteTimeout", &firstByteTimeout_val)) {
      return false;
    }
    if (!Backend::set_timeout_slot(cx, backend, firstByteTimeout_val,
                                   Backend::Slots::FirstByteTimeout, "firstByteTimeout")) {
      return false;
    }
  }

  /// Timeouts for backends must be less than 2^32 milliseconds, or
  /// about a month and a half.
  JS::RootedValue betweenBytesTimeout_val(cx);
  if (!JS_HasProperty(cx, configuration, "betweenBytesTimeout", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "betweenBytesTimeout", &betweenBytesTimeout_val)) {
      return false;
    }
    if (!Backend::set_timeout_slot(cx, backend, betweenBytesTimeout_val,
                                   Backend::Slots::BetweenBytesTimeout, "betweenBytesTimeout")) {
      return false;
    }
  }

  /// Has to be either: 1; 1.1; 1.2; 1.3;
  JS::RootedValue tlsMinVersion_val(cx);
  std::optional<int> tlsMinVersion;
  if (!JS_HasProperty(cx, configuration, "tlsMinVersion", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "tlsMinVersion", &tlsMinVersion_val)) {
      return false;
    }
    double version;
    if (!JS::ToNumber(cx, tlsMinVersion_val, &version)) {
      return false;
    }

    if (isnan(version)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TLS_MIN_INVALID);
      return false;
    }

    if (version == 1.3) {
      tlsMinVersion = fastly::TLS::VERSION_1_3;
    } else if (version == 1.2) {
      tlsMinVersion = fastly::TLS::VERSION_1_2;
    } else if (version == 1.1) {
      tlsMinVersion = fastly::TLS::VERSION_1_1;
    } else if (version == 1) {
      tlsMinVersion = fastly::TLS::VERSION_1;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TLS_MIN_INVALID);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::TlsMinVersion,
                        JS::Int32Value(tlsMinVersion.value()));
  }

  /// Has to be either: 1; 1.1; 1.2; 1.3;
  JS::RootedValue tlsMaxVersion_val(cx);
  std::optional<int> tlsMaxVersion;
  if (!JS_HasProperty(cx, configuration, "tlsMaxVersion", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "tlsMaxVersion", &tlsMaxVersion_val)) {
      return false;
    }
    double version;
    if (!JS::ToNumber(cx, tlsMaxVersion_val, &version)) {
      return false;
    }

    if (isnan(version)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TLS_MAX_INVALID);
      return false;
    }

    if (version == 1.3) {
      tlsMaxVersion = fastly::TLS::VERSION_1_3;
    } else if (version == 1.2) {
      tlsMaxVersion = fastly::TLS::VERSION_1_2;
    } else if (version == 1.1) {
      tlsMaxVersion = fastly::TLS::VERSION_1_1;
    } else if (version == 1) {
      tlsMaxVersion = fastly::TLS::VERSION_1;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_TLS_MAX_INVALID);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::TlsMaxVersion,
                        JS::Int32Value(tlsMaxVersion.value()));
  }

  if (tlsMinVersion.has_value() && tlsMaxVersion.has_value()) {
    if (tlsMinVersion.value() > tlsMaxVersion.value()) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BACKEND_TLS_MIN_GREATER_THAN_TLS_MAX);
      return false;
    }
  }

  JS::RootedValue certificateHostname_val(cx);
  if (!JS_HasProperty(cx, configuration, "certificateHostname", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "certificateHostname", &certificateHostname_val)) {
      return false;
    }
    auto certificateHostname = JS::ToString(cx, certificateHostname_val);
    if (!certificateHostname) {
      return false;
    }

    if (JS_GetStringLength(certificateHostname) == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BACKEND_CERTIFICATE_HOSTNAME_EMPTY);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::CertificateHostname,
                        JS::StringValue(certificateHostname));
  }

  JS::RootedValue useSsl_val(cx);
  if (!JS_HasProperty(cx, configuration, "useSSL", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "useSSL", &useSsl_val)) {
      return false;
    }
    auto value = JS::ToBoolean(useSsl_val);
    JS::SetReservedSlot(backend, Backend::Slots::UseSsl, JS::BooleanValue(value));
  }

  JS::RootedValue caCertificate_val(cx);
  if (!JS_HasProperty(cx, configuration, "caCertificate", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "caCertificate", &caCertificate_val)) {
      return false;
    }
    auto caCertificate = JS::ToString(cx, caCertificate_val);
    if (!caCertificate) {
      return false;
    }
    if (JS_GetStringLength(caCertificate) == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_CA_CERTIFICATE_EMPTY);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::CaCertificate, JS::StringValue(caCertificate));
  }

  /// Cipher list consisting of one or more cipher strings separated by colons.
  // Commas or spaces are also acceptable separators but colons are normally used.
  JS::RootedValue ciphers_val(cx);
  if (!JS_HasProperty(cx, configuration, "ciphers", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "ciphers", &ciphers_val)) {
      return false;
    }
    size_t length;
    auto ciphers_chars = encode(cx, ciphers_val, &length);
    if (!ciphers_chars) {
      return false;
    }
    if (length == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_CIPHERS_EMPTY);
      return false;
    }
    std::string cipherSpec(ciphers_chars.get(), length);
    if (!isCipherSuiteSupportedByFastly(cipherSpec)) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_CIPHERS_NOT_AVALIABLE);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::Ciphers,
                        JS::StringValue(JS_NewStringCopyN(cx, ciphers_chars.get(), length)));
    auto ciphersSlot = JS::GetReservedSlot(backend, Backend::Slots::Ciphers);
    if (!ciphersSlot.isNullOrUndefined()) {
      JS::RootedString ciphers(cx, ciphersSlot.toString());
      size_t ciphers_len;
      JS::UniqueChars ciphersChars = encode(cx, ciphers, &ciphers_len);
    }
  }

  JS::RootedValue sniHostname_val(cx);
  if (!JS_HasProperty(cx, configuration, "sniHostname", &found)) {
    return false;
  }
  if (found) {
    if (!JS_GetProperty(cx, configuration, "sniHostname", &sniHostname_val)) {
      return false;
    }
    auto sniHostname = JS::ToString(cx, sniHostname_val);
    if (!sniHostname) {
      return false;
    }
    if (JS_GetStringLength(sniHostname) == 0) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BACKEND_SNI_HOSTNAME_EMPTY);
      return false;
    }
    JS::SetReservedSlot(backend, Backend::Slots::SniHostname, JS::StringValue(sniHostname));
  }

  auto result = Backend::register_dynamic_backend(cx, backend);
  if (result.isErr()) {
    return false;
  }
  args.rval().setObject(*backend);
  return true;
}

bool Backend::init_class(JSContext *cx, JS::HandleObject global) {
  JS::RootedObject backends(cx, JS_NewPlainObject(cx));
  if (!backends) {
    return false;
  }
  Backend::backends.init(cx, backends);
  return BuiltinImpl<Backend>::init_class_impl(cx, global);
}

} // namespace builtins
