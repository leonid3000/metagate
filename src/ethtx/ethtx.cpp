#include "ethtx.h"

#include <stdlib.h>
#include <string>
#include <vector>

#include <cryptopp/keccak.h>

#include <secp256k1/include/secp256k1_recovery.h>

#include "rlp.h"
#include "utils2.h"
#include "cert.h"
#include "const.h"

#include "check.h"

secp256k1_context const* getCtx()
{
        static std::unique_ptr<secp256k1_context, decltype(&secp256k1_context_destroy)> s_ctx{
                secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY),
                &secp256k1_context_destroy
        };
        return s_ctx.get();
}

//Всем 16ричным строкам предшествует префикс 0x
std::string SignTransaction(std::string rawprivkey,
                            std::string nonce,
                            std::string gasPrice,
                            std::string gasLimit,
                            std::string to,
                            std::string value,
                            std::string data)
{
    CHECK(nonce.find("0x") == 0, "Incorrect nonce " + nonce);
    CHECK(gasPrice.find("0x") == 0, "Incorrect gasPrice " + gasPrice);
    CHECK(gasLimit.find("0x") == 0, "Incorrect gasLimit " + gasLimit);
    CHECK(to.find("0x") == 0, "Incorrect to " + to);
    CHECK(to.size() == 42, "Incorrect to " + to);
    CHECK(value.find("0x") == 0, "Incorrect value " + value);
    if (!data.empty()) {
        CHECK(data.find("0x") == 0, "Incorrect value " + value);
    }
    nonce = nonce.substr(2);
    gasPrice = gasPrice.substr(2);
    gasLimit = gasLimit.substr(2);
    to = to.substr(2);
    value = value.substr(2);
    if (!data.empty()) {
        data = data.substr(2);
    }

    std::vector<std::string> settings;
    settings.push_back(HexStringToDump(nonce));
    settings.push_back(HexStringToDump(gasPrice));
    settings.push_back(HexStringToDump(gasLimit));
    settings.push_back(HexStringToDump(to));
    settings.push_back(HexStringToDump(value));
    settings.push_back(HexStringToDump(data));

    uint8_t hs[EC_KEY_LENGTH];
    std::string rlp = SettingsToRLP(settings);

    CryptoPP::Keccak k(EC_KEY_LENGTH);
    k.Update((uint8_t*)rlp.c_str(), rlp.size());
    k.TruncatedFinal(hs, EC_KEY_LENGTH);

    auto* ctx = getCtx();
    secp256k1_ecdsa_recoverable_signature rawSig;
    const bool res1 = secp256k1_ecdsa_sign_recoverable(ctx, &rawSig, (const unsigned char*)hs, (const unsigned char*)rawprivkey.c_str(), nullptr, nullptr);
    CHECK(res1, "secp256k1_ecdsa_sign_recoverable error");

    uint8_t signature[64] = {0};
    int v = 0;
    const bool res2 = secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx, signature, &v, &rawSig);
    CHECK(res2, "secp256k1_ecdsa_recoverable_signature_serialize_compact error");
    if (v == 0 || v == 1) {
        v += 37;
    }

    std::string vstr;
    vstr.push_back((char)v);

    settings.pop_back();
    settings.pop_back();
    settings.pop_back();

    settings.push_back(vstr);
    settings.push_back(std::string((char*)signature, EC_KEY_LENGTH));
    settings.push_back(std::string((char*)signature + EC_KEY_LENGTH, EC_KEY_LENGTH));
    std::string transaction =  SettingsToRLP(settings, false);
    transaction = "0x" + DumpToHexString((const uint8_t*)transaction.c_str(), transaction.size());

    return transaction;
}
