#include <gtest/gtest.h>

#include "quantBacktest/core/fingerprint.hpp"
#include "quantBacktest/core/random.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace quantBacktest
{

    namespace
    {

        std::string sha256(const std::string &text)
        {
            Fingerprint f;
            f.update(text.data(), text.size());
            return f.hex();
        }

    } // namespace

    // Test vectors of FIPS 180-4 and of NIST's examples.
    TEST(Fingerprint, MatchesTheSha256TestVectors)
    {
        EXPECT_EQ(sha256(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT_EQ(sha256("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT_EQ(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        EXPECT_EQ(sha256(std::string(1000000, 'a')), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    TEST(Fingerprint, DoesNotDependOnHowTheBytesAreCut)
    {
        const std::string text(1000, 'x');
        Fingerprint pieces;
        for (std::size_t k = 0; k < text.size(); k += 7)
        {
            pieces.update(text.data() + k, std::min<std::size_t>(7, text.size() - k));
        }
        EXPECT_EQ(pieces.hex(), sha256(text));

        Fingerprint a;
        a.update(std::uint64_t{0x0102030405060708});
        Fingerprint b;
        const unsigned char bytes[] = {8, 7, 6, 5, 4, 3, 2, 1};
        b.update(bytes, sizeof(bytes));
        EXPECT_EQ(a.hex(), b.hex());
    }

    // The generator is the engine's own: its first draws are fixed for ever,
    // on every platform. A change here breaks every replay.
    TEST(Random, IsReproducibleAndStreamsDiffer)
    {
        // Pinned: these three numbers are the contract.
        Random pinned(42);
        EXPECT_EQ(pinned.next(), 0x85c4ff1bbb7b930eull);
        EXPECT_EQ(pinned.next(), 0xdc1f88cd42f2f8deull);
        EXPECT_EQ(Random(42, 1).next(), 0x4e90cca10015d369ull);

        Random a(42);
        Random b(42);
        Random otherSeed(43);
        Random otherStream(42, 1);
        const std::uint64_t first = a.next();
        EXPECT_EQ(first, b.next());
        EXPECT_NE(first, otherSeed.next());
        EXPECT_NE(first, otherStream.next());

        Random u(7);
        Real sum = 0.0;
        std::vector<Index> counts(10, 0);
        for (int k = 0; k < 100000; ++k)
        {
            const Real x = u.uniform();
            ASSERT_GE(x, 0.0);
            ASSERT_LT(x, 1.0);
            sum += x;
            ++counts[u.uniform_index(10)];
        }
        EXPECT_NEAR(sum / 100000.0, 0.5, 0.005);
        for (const Index count : counts)
        {
            EXPECT_NEAR(static_cast<Real>(count), 10000.0, 400.0);
        }
    }

} // namespace quantBacktest
