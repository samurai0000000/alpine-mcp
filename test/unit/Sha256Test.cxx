/*
 * Sha256Test.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */


#include <cstring>
#include <string>

extern "C" {
#include "mcpjson.h"
#include "mcp.h"
#include "mcpint.h"
}

/* CppUTest after the standard headers: its new macros break them otherwise */
#include "CppUTest/TestHarness.h"

/*
 * SHA-256 is hashed over the canonical arguments of a send or a draft, so
 * a wrong digest would let two different messages share an idempotency
 * key.  Checked against published and independently computed vectors,
 * with every message length around the 64-byte block and the padding
 * boundaries (55, 56, 63, 64, 65, ...).
 */

TEST_GROUP(Sha256)
{
    std::string hex(const std::string &data)
    {
        char out[65];

        memset(out, 'x', sizeof(out));
        mcp_sha256_hex(data.data(), data.size(), out);
        return std::string(out);
    }
};

TEST(Sha256, EmptyMessage)
{
    STRCMP_EQUAL("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
                 hex("").c_str());
}

TEST(Sha256, Abc)
{
    STRCMP_EQUAL("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                 hex("abc").c_str());
}

TEST(Sha256, TwoBlockStandardVector)
{
    STRCMP_EQUAL("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                 hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
}

TEST(Sha256, OneMillionAs)
{
    STRCMP_EQUAL("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                 hex(std::string(1000000, 'a')).c_str());
}

TEST(Sha256, LengthsAroundBlockAndPaddingBoundaries)
{
    static const struct {
        size_t len;
        const char *digest;
    } vectors[] = {
        { 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { 1, "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb" },
        { 55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318" },
        { 56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a" },
        { 57, "f13b2d724659eb3bf47f2dd6af1accc87b81f09f59f2b75e5c0bed6589dfe8c6" },
        { 63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34" },
        { 64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb" },
        { 65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0" },
        { 119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb" },
        { 120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c" },
        { 127, "c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a" },
        { 128, "6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e" },
        { 129, "c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae" },
        { 1000, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3" },
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        STRCMP_EQUAL_TEXT(vectors[i].digest,
                          hex(std::string(vectors[i].len, 'a')).c_str(),
                          (std::string("length ")
                           + std::to_string(vectors[i].len)).c_str());
    }
}

TEST(Sha256, OutputIsLowerCaseHexAndTerminated)
{
    char out[65];

    memset(out, 'x', sizeof(out));
    mcp_sha256_hex("abc", 3, out);
    LONGS_EQUAL(64, strlen(out));
    for (int i = 0; i < 64; i++) {
        CHECK((out[i] >= '0' && out[i] <= '9') || (out[i] >= 'a' && out[i] <= 'f'));
    }
}

TEST(Sha256, EmbeddedNulBytesAreHashed)
{
    std::string a("ab\0cd", 5);
    std::string b("ab\0ce", 5);

    CHECK(hex(a) != hex(b));
    CHECK(hex(a) != hex("ab"));
}

TEST(Sha256, SameInputSameDigestDifferentInputDifferentDigest)
{
    STRCMP_EQUAL(hex("one message").c_str(), hex("one message").c_str());
    CHECK(hex("one message") != hex("one messagf"));
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
