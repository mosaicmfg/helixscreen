// Copyright (C) 2026 356C LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gcode_error_router.h"

#include "helix_test_fixture.h"

#include <string>

#include "../../catch_amalgamated.hpp"

using helix::GcodeErrorRouter;

namespace {
struct GcodeErrorRouterTest : public HelixTestFixture {};
}  // namespace

// clean_error_text must handle K2's two `!!` payload shapes.
//
// Shape 1 — pure JSON (CFS box driver errors, observed key849):
//   `{"code":"key849","msg":"retrude error...","values":[1,"A"]}`
//
// Shape 2 — embedded JSON after a prefix (Klipper connect failure on
// MCU shutdown, observed K2 Plus 2026-05-24 in gcode_store as):
//   `Internal error during connect: !{"code":"key298","msg":"Can not
//    update MCU rpi config as it is shutdown","values":["rpi"]}`
//   `Once the underlying issue is corrected, use the "RESTART"...`
//
// Both shapes must surface `code` so process_line can route to the
// right modal/recovery branch. Shape 2 was missed by the v1 extractor
// (which only parsed when `text[0] == '{'`); user hit it and got a
// generic "Klipper Error" toast instead of the Recover button.

TEST_CASE_METHOD(GcodeErrorRouterTest, "clean_error_text extracts pure JSON shape",
                 "[gcode_error_router]") {
    std::string text =
        R"({"code":"key849","msg":"retrude error, failed to exit connections","values":[1,"A"]})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key849");
    // CFS decoder rewrites to friendly text — exact phrasing depends on
    // CFS_ERROR_TABLE entry; just check the slot locator was spliced in.
    REQUIRE(text.find("unit 1 slot A") != std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text extracts embedded JSON after prefix",
                 "[gcode_error_router]") {
    // Exact shape observed on K2 Plus 2026-05-24 when klipper_mcu was
    // shutdown. The `!` between the prefix and the JSON is Klipper's
    // own emit convention — we don't reproduce it on output.
    std::string text =
        R"(Internal error during connect: !{"code":"key298","msg":"Can not update MCU rpi config as it is shutdown","values":["rpi"]})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code == "key298");
    // CFS decoder has a key298 entry — friendly translation should win
    // over the raw msg.
    REQUIRE(text.find("MCU bridge") != std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text handles embedded JSON with trailing comment garbage",
                 "[gcode_error_router]") {
    // Real K2 emission: the JSON is followed by Klipper's standard
    // "Once the underlying issue is corrected..." footer text.
    std::string text =
        R"(Internal error during connect: !{"code":"key298","msg":"Can not update MCU rpi config as it is shutdown","values":["rpi"]}
//
// Once the underlying issue is corrected, use the "RESTART"
// command to reload the config and restart the host software.)";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    // Brace-balance must terminate at the JSON's close brace and ignore
    // the trailing comment lines.
    REQUIRE(code == "key298");
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text falls through when text has no code",
                 "[gcode_error_router]") {
    std::string text = "Generic Klipper error with no code marker";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code.empty());
    // Text untouched (no heuristic matches either).
    REQUIRE(text == "Generic Klipper error with no code marker");
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text applies must-home heuristic",
                 "[gcode_error_router]") {
    std::string text = "Must home axis first";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code.empty());
    // Rewritten via lv_tr — exact translated string depends on i18n,
    // but the unrelated raw phrasing should be gone.
    REQUIRE(text != "Must home axis first");
    REQUIRE(text.find("home") != std::string::npos);
}

TEST_CASE_METHOD(GcodeErrorRouterTest,
                 "clean_error_text recovers from malformed JSON after prefix",
                 "[gcode_error_router]") {
    // Embedded `{"code":` substring but with broken JSON (missing close
    // quote). Brace-balance can still find an end, but parse must throw
    // and we fall through without leaking partial state.
    std::string text = R"(Internal error: {"code":"key298, "msg":"broken"})";
    std::string code;
    GcodeErrorRouter::clean_error_text(text, code);

    REQUIRE(code.empty());
    // Text is unmodified because parse threw before the friendly rewrite.
    REQUIRE(text.find("Internal error") != std::string::npos);
}
