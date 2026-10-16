/*
 * text_test.cpp -- the wording's device-free checks, run by `--text-test <dir>` before any host starts: fmt()
 * against texts written here, each compared with the exact string it must give, and the count of logged problems
 * before and after, so "logged once" is checked and not only "drawn". One line a check in <dir>/text_test.txt and
 * in the log, then the total.
 */
#include "text_test.h"

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../app/log.h"
#include "text.h"
#include "ui/textstrings.h"

namespace vrcore::ui {

namespace {

struct Report {
    std::FILE *f = nullptr;
    int pass = 0, fail = 0;

    void check(bool ok, const char *what, ...)
    {
        char buf[256];
        va_list ap;
        va_start(ap, what);
        std::vsnprintf(buf, sizeof buf, what, ap);
        va_end(ap);
        ++(ok ? pass : fail);
        log("text-test: %s %s", ok ? "PASS" : "FAIL", buf);
        if (f) std::fprintf(f, "%s %s\n", ok ? "PASS" : "FAIL", buf);
    }

    // fmt()'s result is `want`, and it logged `logs` new problems doing so
    void same(const std::string &got, const char *want, int logs, int before, const char *name)
    {
        int now = fmt_logged() - before;
        check(got == want && now == logs, "%s: \"%s\" (want \"%s\"), %d logged (want %d)", name, got.c_str(), want,
              now, logs);
    }
};

}

int text_test(const char *dir)
{
    Report r;
    std::string out = std::string(dir) + "/text_test.txt";
    r.f = std::fopen(out.c_str(), "w");
    if (!r.f) log("text-test: cannot write %s", out.c_str());

    int n = fmt_logged();
    r.same(fmt("Return"), "Return", 0, n, "no slots");
    n = fmt_logged();
    r.same(fmt(nullptr), "", 0, n, "nullptr text");
    n = fmt_logged();
    r.same(fmt(""), "", 0, n, "empty text");
    n = fmt_logged();
    r.same(fmt("You need {file}", {{"file", "game.zip"}}), "You need game.zip", 0, n, "one slot");
    n = fmt_logged();
    r.same(fmt("{a}{b} and {a}", {{"b", "2"}, {"a", "1"}}), "12 and 1", 0, n, "slots in any order, one used twice");
    n = fmt_logged();
    std::string file = "rom.bin";
    r.same(fmt("{v} m, {file}", {{"v", std::to_string(3)}, {"file", file}}), "3 m, rom.bin", 0, n,
           "values from temporaries and std::string");
    n = fmt_logged();
    r.same(fmt("{{literal}} and }} {{", {{"literal", "x"}}), "{literal} and } {", 1, n,
           "doubled braces are literal (and the slot passed is unused)");
    n = fmt_logged();
    r.same(fmt("{{{v}}}", {{"v", "7"}}), "{7}", 0, n, "a slot inside literal braces");
    n = fmt_logged();
    r.same(fmt("One more: {file}"), "One more: {file?}", 1, n, "a missing slot is shown and logged");
    n = fmt_logged();
    r.same(fmt("One more: {file}"), "One more: {file?}", 0, n, "the same missing slot is not logged again");
    n = fmt_logged();
    r.same(fmt("Two more: {file}"), "Two more: {file?}", 1, n, "the same slot in another text is logged");
    n = fmt_logged();
    r.same(fmt("Hi", {{"name", "x"}}), "Hi", 1, n, "an unused value is logged");
    n = fmt_logged();
    r.same(fmt("Hi", {{"name", "x"}}), "Hi", 0, n, "the same unused value is not logged again");
    n = fmt_logged();
    r.same(fmt("a { b } c {"), "a { b } c {", 0, n, "lone braces and { b } are copied");
    n = fmt_logged();
    r.same(fmt("open {x{y} end", {{"y", "Y"}}), "open {xY end", 0, n, "a { before a slot is copied");
    n = fmt_logged();
    r.same(fmt("tail {"), "tail {", 0, n, "a { at the end is copied");
    n = fmt_logged();
    r.same(fmt("{} {File} {a1}"), "{} {File} {a1}", 0, n, "braces round a non-name are copied");
    n = fmt_logged();
    r.same(fmt(txt::menu_confirm, {{"label", "Restart"}}), "Confirm Restart", 0, n,
           "a constant from ui/textstrings.h");

    log("text-test: %d passed, %d failed", r.pass, r.fail);
    if (r.f) {
        std::fprintf(r.f, "total %d passed, %d failed\n", r.pass, r.fail);
        std::fclose(r.f);
    }
    return r.fail == 0 ? 0 : 1;
}

}
