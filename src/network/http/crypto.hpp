#pragma once
// Ported from Simple-Web-Server (https://github.com/eidheim/Simple-Web-Server)
// MIT License - Copyright (c) 2014-2018 Ole Christian Eidheim

#include <cmath>
#include <iomanip>
#include <istream>
#include <sstream>
#include <string>
#include <vector>

#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/sha.h>

namespace gb::http {

  class Crypto {
    const static std::size_t buffer_size = 131072;

  public:
    class Base64 {
    public:
      static std::string encode(const std::string &ascii) noexcept {
        std::string base64;

        BIO *bio, *b64;
        BUF_MEM *bptr = BUF_MEM_new();

        b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_new(BIO_s_mem());
        BIO_push(b64, bio);
        BIO_set_mem_buf(b64, bptr, BIO_CLOSE);

        // 直接写入 base64 缓冲区，避免拷贝
        auto base64_length = static_cast<std::size_t>(round(4 * ceil(static_cast<double>(ascii.size()) / 3.0)));
        base64.resize(base64_length);
        bptr->length = 0;
        bptr->max = base64_length + 1;
        bptr->data = &base64[0];

        if(BIO_write(b64, &ascii[0], static_cast<int>(ascii.size())) <= 0 || BIO_flush(b64) <= 0)
          base64.clear();

        // 在 BIO_free_all(b64) 期间保持 &base64[0] 有效
        bptr->length = 0;
        bptr->max = 0;
        bptr->data = nullptr;

        BIO_free_all(b64);

        return base64;
      }

      static std::string decode(const std::string &base64) noexcept {
        std::string ascii;

        // 调整 ascii 的大小，不过该大小最多会比实际多出两个字节
        ascii.resize((6 * base64.size()) / 8);
        BIO *b64, *bio;

        b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        bio = BIO_new_mem_buf(&base64[0], static_cast<int>(base64.size()));
        bio = BIO_push(b64, bio);

        auto decoded_length = BIO_read(bio, &ascii[0], static_cast<int>(ascii.size()));
        if(decoded_length > 0)
          ascii.resize(static_cast<std::size_t>(decoded_length));
        else
          ascii.clear();

        BIO_free_all(b64);

        return ascii;
      }
    };

    /// 返回输入字符串各字节的十六进制字符串。
    static std::string to_hex_string(const std::string &input) noexcept {
      std::stringstream hex_stream;
      hex_stream << std::hex << std::internal << std::setfill('0');
      for(auto &byte : input)
        hex_stream << std::setw(2) << static_cast<int>(static_cast<unsigned char>(byte));
      return hex_stream.str();
    }

    static std::string md5(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(128 / 8);
      MD5(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string md5(std::istream &stream, std::size_t iterations = 1) noexcept {
      MD5_CTX context;
      MD5_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        MD5_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(128 / 8);
      MD5_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        MD5(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha1(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(160 / 8);
      SHA1(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha1(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA_CTX context;
      SHA1_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA1_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(160 / 8);
      SHA1_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA1(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha256(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(256 / 8);
      SHA256(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha256(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA256_CTX context;
      SHA256_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(256 / 8);
      SHA256_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA256(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha512(const std::string &input, std::size_t iterations = 1) noexcept {
      std::string hash;

      hash.resize(512 / 8);
      SHA512(reinterpret_cast<const unsigned char *>(&input[0]), input.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      for(std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    static std::string sha512(std::istream &stream, std::size_t iterations = 1) noexcept {
      SHA512_CTX context;
      SHA512_Init(&context);
      std::streamsize read_length;
      std::vector<char> buffer(buffer_size);
      while((read_length = stream.read(&buffer[0], buffer_size).gcount()) > 0)
        SHA512_Update(&context, buffer.data(), static_cast<std::size_t>(read_length));
      std::string hash;
      hash.resize(512 / 8);
      SHA512_Final(reinterpret_cast<unsigned char *>(&hash[0]), &context);

      for(std::size_t c = 1; c < iterations; ++c)
        SHA512(reinterpret_cast<const unsigned char *>(&hash[0]), hash.size(), reinterpret_cast<unsigned char *>(&hash[0]));

      return hash;
    }

    /// key_size 为返回密钥的字节数。
    static std::string pbkdf2(const std::string &password, const std::string &salt, int iterations, int key_size) noexcept {
      std::string key;
      key.resize(static_cast<std::size_t>(key_size));
      PKCS5_PBKDF2_HMAC_SHA1(password.c_str(), password.size(),
                             reinterpret_cast<const unsigned char *>(salt.c_str()), salt.size(), iterations,
                             key_size, reinterpret_cast<unsigned char *>(&key[0]));
      return key;
    }
  };
}
