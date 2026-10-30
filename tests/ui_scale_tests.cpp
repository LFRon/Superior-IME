// SPDX-License-Identifier: GPL-3.0-or-later
#include <string_view>

#include "platform/ScreenScale.hpp"
#include "support/TestSupport.hpp"

namespace {

using superior::ui::parseXftDpi;
using superior::ui::scaleForWindows;
using superior::ui::ScaleSource;
using superior::ui::scaleSourceName;
using superior::ui::SystemScale;

/// `xrdb -query` output is a multi-line resource database; only one line says
/// how big the desktop is.
void testXftDpiParsing() {
    CHECK(parseXftDpi("Xft.dpi:\t120\nXcursor.size:\t24\n") == 120);
    CHECK(parseXftDpi("Xcursor.theme:\tbloom\nXft.dpi: 96\n") == 96);
    CHECK(parseXftDpi("Xft.dpi:144") == 144);
    CHECK(parseXftDpi("  Xft.dpi:\t  168  \n") == 168);
}

/// A missing or nonsense value must mean "the desktop is 1:1", never a window
/// nobody can use. `Xft.dpi: 0` and a runaway line are typos, not scaling.
void testXftDpiRejectsNonsense() {
    CHECK(parseXftDpi("") == 0);
    CHECK(parseXftDpi("Xcursor.size: 24") == 0);
    CHECK(parseXftDpi("Xft.dpi: 0") == 0);
    CHECK(parseXftDpi("Xft.dpi: 4") == 0);
    CHECK(parseXftDpi("Xft.dpi: 100000") == 0);
    CHECK(parseXftDpi("Xft.dpi: abc") == 0);
    // A different resource must not be mistaken for the one we asked for.
    CHECK(parseXftDpi("Xft.dpi.bogus: 200") == 0);
}

/// The toolkit already applies what it cached at start-up; the ratio handed to
/// the windows is what is left, so the final size is system × user exactly once.
void testCompensation() {
    const SystemScale one{};
    CHECK(superior::test::near(scaleForWindows(one, 1.0, 1.0), 1.0));

    // The common case: the desktop says 1.25 and Qt already drew at 1.25.
    const SystemScale x125{1.25, ScaleSource::X11XftDpi, 120};
    CHECK(superior::test::near(scaleForWindows(x125, 1.25, 1.0), 1.0));
    // The user asks for 150% on top of a 125% desktop.
    CHECK(superior::test::near(scaleForWindows(x125, 1.25, 1.5), 1.5));

    // Qt was born at 1x and the desktop has since been set to 1.5: the overlay
    // has to grow by itself, because Qt never re-reads the scale.
    const SystemScale x150{1.5, ScaleSource::X11XftDpi, 144};
    CHECK(superior::test::near(scaleForWindows(x150, 1.0, 1.0), 1.5));
    // …and it lands on exactly 1.5 even while Qt is stale at 1.25.
    CHECK(superior::test::near(scaleForWindows(x150, 1.25, 1.0), 1.2));

    // Wayland reports an integer output scale.
    const SystemScale wayland2{2.0, ScaleSource::WaylandOutput, 2};
    CHECK(superior::test::near(scaleForWindows(wayland2, 2.0, 1.0), 1.0));
    CHECK(superior::test::near(scaleForWindows(wayland2, 1.0, 1.0), 2.0));

    // Garbage in the settings file must not produce a zero-sized window.
    CHECK(superior::test::near(scaleForWindows(x125, 0.0, 0.0), 1.25));
}

void testSourceNames() {
    CHECK(std::string_view(scaleSourceName(ScaleSource::X11XftDpi)) == "x11-xft-dpi");
    CHECK(std::string_view(scaleSourceName(ScaleSource::WaylandOutput)) == "wayland-output");
    CHECK(std::string_view(scaleSourceName(ScaleSource::Fallback)) == "fallback");
}

} // namespace

int main() {
    testXftDpiParsing();
    testXftDpiRejectsNonsense();
    testCompensation();
    testSourceNames();
    return superior::test::finish("ui scale tests");
}
