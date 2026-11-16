// Direct tests for the media-mode transition on server_tokens.
//
// These call the real production function rather than re-implementing it: the logic
// under test *is* the logic, and a copy of it would pass while the real one stayed
// wrong.
//
// What this covers, and what it deliberately does not:
//
//   covered   - a media-free container (the shape the tokenizer produces for a
//               text-only request) adopting a media-capable server's mode, and the
//               operations that must keep working across that change: the tokens
//               themselves, pos_next, keep_first, clear, clone, get_common_prefix.
//   not covered - a container that *holds* media, because building one requires real
//               mtmd chunks from a real mmproj. Those cases are exercised end to end
//               against the running server instead (see change.md), and the unit test
//               does not pretend otherwise.

#include "server-common.h"

#include <cstdio>
#include <string>
#include <vector>

static int n_failed = 0;

#define CHECK(cond) do { \
        if (!(cond)) { \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            n_failed++; \
        } \
    } while (0)

// plain text tokens, built exactly the way the tokenizer builds a text-only request:
// the flag is false whatever the server can do
static server_tokens make_text_tokens(size_t n) {
    llama_tokens toks;
    toks.reserve(n);
    for (size_t i = 0; i < n; i++) {
        toks.push_back((llama_token) (1000 + i));
    }
    return server_tokens(toks, false);
}

// ---------------------------------------------------------------------------
// the transition that matters: text-marked container, media-capable server
// ---------------------------------------------------------------------------

void test_text_container_is_promoted_to_a_media_server() {
    server_tokens t = make_text_tokens(64);

    CHECK(t.has_mtmd == false);

    CHECK(t.bind_media_mode(true) == true);
    CHECK(t.has_mtmd == true);

    // the tokens are untouched, which is what makes the promotion safe to apply to a
    // container whose KV has already been restored from disk
    CHECK(t.size() == 64);
    CHECK(t[0]    == 1000);
    CHECK(t[63]   == 1063);
}

void test_text_container_is_left_alone_on_a_text_server() {
    server_tokens t = make_text_tokens(32);

    CHECK(t.bind_media_mode(false) == true);
    CHECK(t.has_mtmd == false);
    CHECK(t.size() == 32);
}

void test_promotion_is_idempotent_and_reversible_when_media_free() {
    server_tokens t = make_text_tokens(16);

    CHECK(t.bind_media_mode(true)  == true);
    CHECK(t.bind_media_mode(true)  == true);
    CHECK(t.bind_media_mode(false) == true);

    // a container holding no media simply follows the server; what must never happen is
    // a refusal for a container that has nothing media-related in it
    CHECK(t.has_mtmd == false);
    CHECK(t.size() == 16);
}

void test_empty_container_follows_the_server() {
    server_tokens a = make_text_tokens(0);
    CHECK(a.bind_media_mode(true) == true);
    CHECK(a.has_mtmd == true);

    server_tokens b = make_text_tokens(0);
    CHECK(b.bind_media_mode(false) == true);
    CHECK(b.has_mtmd == false);
}

// ---------------------------------------------------------------------------
// the operations that must keep working across a mode change
// ---------------------------------------------------------------------------

void test_pos_next_is_unaffected_by_the_flag() {
    // For a container with no media entries pos_next is the token count either way.
    // If promoting the container changed this, every position computed for a restored
    // prompt would shift.
    server_tokens t = make_text_tokens(30);

    CHECK(t.pos_next() == 30);

    CHECK(t.bind_media_mode(true) == true);

    CHECK(t.pos_next() == 30);
}

void test_keep_first_truncates_normally_after_promotion() {
    server_tokens t = make_text_tokens(40);
    CHECK(t.bind_media_mode(true) == true);

    t.keep_first(10);

    // with no media entries this is plain token truncation
    CHECK(t.size() == 10);
    CHECK(t.has_mtmd == true);
    CHECK(t[9] == 1009);
}

void test_clear_keeps_the_capability_flag() {
    // clear() resets the tokens and the media map. The flag describes the container's
    // relationship to the server rather than its current contents, so it survives -
    // and a cleared prompt must still be usable for a media request.
    server_tokens t = make_text_tokens(16);
    CHECK(t.bind_media_mode(true) == true);

    t.clear();

    CHECK(t.size() == 0);
    CHECK(t.n_media_chunks() == 0);
    CHECK(t.has_mtmd == true);
}

void test_clone_carries_the_flag_and_stays_independent() {
    server_tokens t = make_text_tokens(24);
    CHECK(t.bind_media_mode(true) == true);

    server_tokens c = t.clone();

    CHECK(c.has_mtmd == true);
    CHECK(c.size() == t.size());
    CHECK(c.n_media_chunks() == t.n_media_chunks());

    // re-binding the copy must not disturb the original
    CHECK(c.bind_media_mode(false) == true);
    CHECK(c.has_mtmd == false);
    CHECK(t.has_mtmd == true);
}

void test_common_prefix_survives_the_mode_change() {
    // A promoted container must still match plain text tokens. If the promotion changed
    // how the prefix is compared, a restored prompt would stop matching the request
    // that reuses it - the cache would silently stop working rather than fail loudly.
    server_tokens cached = make_text_tokens(48);
    CHECK(cached.bind_media_mode(true) == true);

    server_tokens request = make_text_tokens(48);
    CHECK(request.bind_media_mode(true) == true);

    CHECK(cached.get_common_prefix(request) == 48);

    // a request that diverges shares only up to the divergence
    llama_tokens diverging_toks;
    for (size_t i = 0; i < 48; i++) {
        diverging_toks.push_back((llama_token) (i < 20 ? 1000 + i : 5000 + i));
    }
    server_tokens diverging(diverging_toks, true);

    CHECK(cached.get_common_prefix(diverging) == 20);
}

void test_a_shorter_request_against_a_promoted_container() {
    // The realistic reuse shape: a long restored text prompt, then a request that is a
    // prefix of it. This is how a cache hit is measured, and the promotion must not
    // disturb it.
    server_tokens cached = make_text_tokens(100);
    CHECK(cached.bind_media_mode(true) == true);

    server_tokens shorter = make_text_tokens(25);
    CHECK(shorter.bind_media_mode(true) == true);

    CHECK(cached.get_common_prefix(shorter) == 25);
}

int main() {
    test_text_container_is_promoted_to_a_media_server();
    test_text_container_is_left_alone_on_a_text_server();
    test_promotion_is_idempotent_and_reversible_when_media_free();
    test_empty_container_follows_the_server();
    test_pos_next_is_unaffected_by_the_flag();
    test_keep_first_truncates_normally_after_promotion();
    test_clear_keeps_the_capability_flag();
    test_clone_carries_the_flag_and_stays_independent();
    test_common_prefix_survives_the_mode_change();
    test_a_shorter_request_against_a_promoted_container();

    if (n_failed == 0) {
        std::printf("all media-transition checks passed\n");
        return 0;
    }

    std::fprintf(stderr, "%d check(s) failed\n", n_failed);
    return 1;
}
