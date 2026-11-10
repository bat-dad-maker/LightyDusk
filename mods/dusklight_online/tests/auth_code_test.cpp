#include "dusklight_online/net/auth_code.hpp"

#include <cassert>
#include <string>

using namespace dusklight_online::net::auth;

int main() {
    // RFC 4231 test case 1 and RFC 6070's PBKDF2-HMAC-SHA256 equivalent.
    const std::string key(20, '\x0b');
    assert(hex(hmac_sha256(key, "Hi There")) ==
        "b0344c61d8db38535ca8afceaf0bf12b"
        "881dc200c9833da726e9376c2e32cff7");
    assert(hex(pbkdf2_sha256("password", "salt", 1)) ==
        "120fb6cffcf8b32c43e7225256c4f837"
        "a86548c92ccc35480805987cb70be17b");
    assert(hex(pbkdf2_sha256("password", "salt", 2)) ==
        "ae4d0c95af6b46d32d0adff928f06dd0"
        "2a303f8ef3c251dfd6e2d85a95474c43");
    const auto verifier = lobby_verifier("secret", "Test Room", "endpoint-secret");
    assert(hex(verifier) ==
        "be1a3f125fb0528382dcd4ac335c21c0"
        "8193f51355f2f04c029a708b0e7372e2");
    assert(verifier == lobby_verifier("secret", "test room", "endpoint-secret"));
    assert(verifier != lobby_verifier("wrong", "test room", "endpoint-secret"));
    const auto proofKey = room_proof_key(verifier, "00112233445566778899aabbccddeeff");
    assert(proofKey.has_value());
    assert(hex(*proofKey) ==
        "a0f9e442418d67ea6622712aefdd965c"
        "c87fd183570203d424d68174ce8c47e5");
    assert(!room_proof_key(verifier, "invalid").has_value());
    const std::string nonce = "ffeeddccbbaa99887766554433221100";
    assert(room_proof(*proofKey, nonce) ==
        "2f326a66638d3056076da6f1bfad2c6"
        "443095bd933c01196ac152507dac02be5");
    assert(room_proof(*proofKey, nonce) != room_proof(*proofKey,
        "00112233445566778899aabbccddeeff"));
    assert(direct_proof("session-a", nonce) != direct_proof("session-b", nonce));
    assert(!equal(direct_proof("session-a", nonce), direct_proof("session-b", nonce)));
    assert(random_nonce().size() == 32);
}
