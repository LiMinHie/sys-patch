#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one
#define STBTT_STATIC
#include <tesla.hpp>    // The Tesla Header
#include <string>
#include <string_view>
#include <utility>
#include <cstddef>
#include "minIni/minIni.h"

namespace {

constexpr auto CONFIG_PATH = "/config/sys-patch/config.ini";
constexpr auto LOG_PATH = "/config/sys-patch/log.ini";

// Optimized: Use structured binding instead of pair
auto split_log_value(std::string_view value) -> std::pair<std::string, std::string> {
    if (value.empty()) {
        return {};
    }

    const auto offset_start = value.rfind(" (0x");
    if (offset_start == std::string_view::npos || value.back() != ')') {
        return {std::string{value}, {}};
    }

    return {
        std::string{value.substr(0, offset_start)},
        std::string{value.substr(offset_start + 1, value.size() - offset_start - 2)}
    };
}

// Optimized: Cache file system operations
class FileSystemHelper {
private:
    mutable FsFileSystem fs;
    mutable bool fs_initialized = false;

    auto ensure_fs() const -> bool {
        if (!fs_initialized) {
            fs_initialized = R_SUCCEEDED(fsOpenSdCardFileSystem(&fs));
        }
        return fs_initialized;
    }

public:
    ~FileSystemHelper() {
        if (fs_initialized) {
            fsFsClose(&fs);
        }
    }

    auto does_file_exist(const char* path) const -> bool {
        if (!ensure_fs()) return false;
        
        FsFile file{};
        char path_buf[FS_MAX_PATH]{};
        strcpy(path_buf, path);
        
        Result rc = fsFsOpenFile(&fs, path_buf, FsOpenMode_Read, &file);
        if (R_SUCCEEDED(rc)) {
            fsFileClose(&file);
        }
        return R_SUCCEEDED(rc);
    }

    auto create_dir(const char* path) const -> bool {
        if (!ensure_fs()) return false;
        
        char path_buf[FS_MAX_PATH]{};
        strcpy(path_buf, path);
        return R_SUCCEEDED(fsFsCreateDirectory(&fs, path_buf));
    }
};

// Global file system helper (lazy initialized)
FileSystemHelper g_fs_helper;

class ColouredListItem : public tsl::elm::ListItem {
public:
    ColouredListItem(const std::string& text, const std::string& value, tsl::Color value_colour)
        : tsl::elm::ListItem(text, value), m_value_colour(value_colour) {
    }

    void draw(tsl::gfx::Renderer *renderer) override {
        if (this->m_touched && Element::getInputMode() == tsl::InputMode::Touch) {
            renderer->drawRect(ELEMENT_BOUNDS(this), a(tsl::style::color::ColorClickAnimation));
        }

        if (this->m_maxWidth == 0) {
            if (!this->m_value.empty()) {
                auto [valueWidth, valueHeight] = renderer->drawString(this->m_value.c_str(), false, 0, 0, 20, tsl::style::color::ColorTransparent);
                this->m_maxWidth = this->getWidth() - valueWidth - 70;
            } else {
                this->m_maxWidth = this->getWidth() - 40;
            }

            auto [width, height] = renderer->drawString(this->m_text.c_str(), false, 0, 0, 23, tsl::style::color::ColorTransparent);
            this->m_trunctuated = width > this->m_maxWidth;

            if (this->m_trunctuated) {
                this->m_scrollText = this->m_text + "        ";
                auto [scrollWidth, scrollHeight] = renderer->drawString(this->m_scrollText.c_str(), false, 0, 0, 23, tsl::style::color::ColorTransparent);
                this->m_scrollText += this->m_text;
                this->m_textWidth = scrollWidth;
                this->m_ellipsisText = renderer->limitStringLength(this->m_text, false, 22, this->m_maxWidth);
            } else {
                this->m_textWidth = width;
            }
        }

        renderer->drawRect(this->getX(), this->getY(), this->getWidth(), 1, a(tsl::style::color::ColorFrame));
        renderer->drawRect(this->getX(), this->getTopBound(), this->getWidth(), 1, a(tsl::style::color::ColorFrame));

        if (this->m_trunctuated) {
            if (this->m_focused) {
                renderer->enableScissoring(this->getX(), this->getY(), this->m_maxWidth + 40, this->getHeight());
                renderer->drawString(this->m_scrollText.c_str(), false, this->getX() + 20 - this->m_scrollOffset, this->getY() + 45, 23, tsl::style::color::ColorText);
                renderer->disableScissoring();
                if (this->m_scrollAnimationCounter == 90) {
                    if (this->m_scrollOffset == this->m_textWidth) {
                        this->m_scrollOffset = 0;
                        this->m_scrollAnimationCounter = 0;
                    } else {
                        this->m_scrollOffset++;
                    }
                } else {
                    this->m_scrollAnimationCounter++;
                }
            } else {
                renderer->drawString(this->m_ellipsisText.c_str(), false, this->getX() + 20, this->getY() + 45, 23, a(tsl::style::color::ColorText));
            }
        } else {
            renderer->drawString(this->m_text.c_str(), false, this->getX() + 20, this->getY() + 45, 23, a(tsl::style::color::ColorText));
        }

        renderer->drawString(this->m_value.c_str(), false, this->getX() + this->m_maxWidth + 45, this->getY() + 45, 20, a(m_value_colour));
    }

private:
    tsl::Color m_value_colour;
};

// Optimized: ConfigEntry with better memory layout and const correctness
struct ConfigEntry {
    const char* const section;
    const char* const key;
    bool value;

    ConfigEntry(const char* _section, const char* _key, bool default_value) noexcept
        : section{_section}, key{_key}, value{default_value} {
            load_value_from_ini();
        }

    void load_value_from_ini() noexcept {
        value = ini_getbool(section, key, value, CONFIG_PATH);
    }

    auto create_list_item(const char* text) noexcept {
        auto item = new tsl::elm::ToggleListItem(text, value);
        item->setStateChangedListener([this](bool new_value){
            value = new_value;
            ini_putl(section, key, value, CONFIG_PATH);
        });
        return item;
    }
};

// Optimized: Color definitions as constexpr in a namespace
namespace colors {
    constexpr tsl::Color syspatch{0, 255, 200, 255};
    constexpr tsl::Color file{255, 177, 66, 255};
    constexpr tsl::Color unpatched{250, 90, 58, 255};
}

// Optimized: Separate configuration entries into a dedicated structure
struct AllPatchConfigs {
    // Options
    ConfigEntry patch_sysmmc{"options", "patch_sysmmc", true};
    ConfigEntry patch_emummc{"options", "patch_emummc", true};
    ConfigEntry logging{"options", "enable_logging", true};
    ConfigEntry version_skip{"options", "version_skip", true};

    // FS patches
    ConfigEntry noacidsigchk1{"fs", "noacidsigchk_1.0.0-9.2.0", true};
    ConfigEntry noacidsigchk2{"fs", "noacidsigchk_1.0.0-9.2.0", true};
    ConfigEntry noncasigchk1{"fs", "noncasigchk_1.0.0-3.0.2", true};
    ConfigEntry noncasigchk2{"fs", "noncasigchk_4.0.0-16.1.0", true};
    ConfigEntry noncasigchk3{"fs", "noncasigchk_17.0.0+", true};
    ConfigEntry nocntchk1{"fs", "nocntchk_1.0.0-18.1.0", true};
    ConfigEntry nocntchk2{"fs", "nocntchk_19.0.0+", true};

    // LDR patches
    ConfigEntry noacidsigchk4{"ldr", "noacidsigchk_10.0.0+", true};

    // ERPT patches
    ConfigEntry no_erpt{"erpt", "no_erpt", true};

    // ES patches
    ConfigEntry es1{"es", "es_1.0.0-8.1.1", true};
    ConfigEntry es2{"es", "es_9.0.0-11.0.1", true};
    ConfigEntry es3{"es", "es_12.0.0-18.1.0", true};
    ConfigEntry es4{"es", "es_19.0.0-21.2.0", true};
    ConfigEntry es5{"es", "es_22.0.0+", true};

    // AM patches
    ConfigEntry am1{"am", "am_homebrew_fix_22.0.0+", true};

    // OLSC patches
    ConfigEntry olsc1{"olsc", "olsc_6.0.0-14.1.2", true};
    ConfigEntry olsc2{"olsc", "olsc_15.0.0-18.1.0", true};
    ConfigEntry olsc3{"olsc", "olsc_19.0.0+", true};

    // NIFM patches
    ConfigEntry ctest1{"nifm", "ctest_1.0.0-19.0.1", true};
    ConfigEntry ctest2{"nifm", "ctest_20.0.0+", true};

    // NIM patches
    ConfigEntry nim1{"nim", "blankcal0crashfix_17.0.0+", true};
    ConfigEntry nim_fw1{"nim", "blockfirmwareupdates_1.0.0-5.1.0", true};
    ConfigEntry nim_fw2{"nim", "blockfirmwareupdates_6.0.0-6.2.0", true};
    ConfigEntry nim_fw3{"nim", "blockfirmwareupdates_7.0.0-10.2.0", true};
    ConfigEntry nim_fw4{"nim", "blockfirmwareupdates_11.0.0-11.0.1", true};
    ConfigEntry nim_fw5{"nim", "blockfirmwareupdates_12.0.0+", true};

    // NS patches
    ConfigEntry ns1{"ns", "force_gamecard_region_to_global", true};
};

class GuiOptions final : public tsl::Gui {
private:
    AllPatchConfigs& m_config;

public:
    GuiOptions(AllPatchConfigs& config) : m_config(config) { }

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("sys-patch", VERSION_WITH_HASH);
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Options"));
        list->addItem(m_config.patch_sysmmc.create_list_item("Patch sysMMC"));
        list->addItem(m_config.patch_emummc.create_list_item("Patch emuMMC"));
        list->addItem(m_config.logging.create_list_item("Logging"));
        list->addItem(m_config.version_skip.create_list_item("Version skip"));

        frame->setContent(list);
        return frame;
    }
};

class GuiToggle final : public tsl::Gui {
private:
    AllPatchConfigs& m_config;

public:
    GuiToggle(AllPatchConfigs& config) : m_config(config) { }

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("sys-patch", VERSION_WITH_HASH);
        auto list = new tsl::elm::List();

        // FS patches
        list->addItem(new tsl::elm::CategoryHeader("FS - 0100000000000000"));
        list->addItem(m_config.noacidsigchk1.create_list_item("noacidsigchk_1.0.0-9.2.0"));
        list->addItem(m_config.noacidsigchk2.create_list_item("noacidsigchk_1.0.0-9.2.0"));
        list->addItem(m_config.noncasigchk1.create_list_item("noncasigchk_1.0.0-3.0.2"));
        list->addItem(m_config.noncasigchk2.create_list_item("noncasigchk_4.0.0-16.1.0"));
        list->addItem(m_config.noncasigchk3.create_list_item("noncasigchk_17.0.0+"));
        list->addItem(m_config.nocntchk1.create_list_item("nocntchk_1.0.0-18.1.0"));
        list->addItem(m_config.nocntchk2.create_list_item("nocntchk_19.0.0+"));

        // LDR patches
        list->addItem(new tsl::elm::CategoryHeader("LDR - 0100000000000001"));
        list->addItem(m_config.noacidsigchk4.create_list_item("noacidsigchk_10.0.0+"));

        // ERPT patches
        list->addItem(new tsl::elm::CategoryHeader("ERPT - 010000000000002B"));
        list->addItem(m_config.no_erpt.create_list_item("no_erpt"));

        // ES patches
        list->addItem(new tsl::elm::CategoryHeader("ES - 0100000000000033"));
        list->addItem(m_config.es1.create_list_item("es_1.0.0-8.1.1"));
        list->addItem(m_config.es2.create_list_item("es_9.0.0-11.0.1"));
        list->addItem(m_config.es3.create_list_item("es_12.0.0-18.1.0"));
        list->addItem(m_config.es4.create_list_item("es_19.0.0-21.2.0"));
        list->addItem(m_config.es5.create_list_item("es_22.0.0+"));

        // AM patches
        list->addItem(new tsl::elm::CategoryHeader("AM - 0100000000000023"));
        list->addItem(m_config.am1.create_list_item("am_homebrew_fix_22.0.0+"));

        // OLSC patches
        list->addItem(new tsl::elm::CategoryHeader("OLSC - 010000000000003E"));
        list->addItem(m_config.olsc1.create_list_item("olsc_6.0.0-14.1.2"));
        list->addItem(m_config.olsc2.create_list_item("olsc_15.0.0-18.1.0"));
        list->addItem(m_config.olsc3.create_list_item("olsc_19.0.0+"));

        // NIFM patches
        list->addItem(new tsl::elm::CategoryHeader("NIFM - 010000000000000F"));
        list->addItem(m_config.ctest1.create_list_item("ctest_1.0.0-19.0.1"));
        list->addItem(m_config.ctest2.create_list_item("ctest_20.0.0+"));

        // NIM patches
        list->addItem(new tsl::elm::CategoryHeader("NIM - 0100000000000025"));
        list->addItem(m_config.nim1.create_list_item("blankcal0crashfix_17.0.0+"));
        list->addItem(m_config.nim_fw1.create_list_item("blockfirmwareupdates_1.0.0-5.1.0"));
        list->addItem(m_config.nim_fw2.create_list_item("blockfirmwareupdates_6.0.0-6.2.0"));
        list->addItem(m_config.nim_fw3.create_list_item("blockfirmwareupdates_7.0.0-10.2.0"));
        list->addItem(m_config.nim_fw4.create_list_item("blockfirmwareupdates_11.0.0-11.0.1"));
        list->addItem(m_config.nim_fw5.create_list_item("blockfirmwareupdates_12.0.0+"));

        // NS patches
        list->addItem(new tsl::elm::CategoryHeader("NS - 010000000000001F"));
        list->addItem(m_config.ns1.create_list_item("force_gamecard_region_to_global"));

        frame->setContent(list);
        return frame;
    }
};

class GuiLog final : public tsl::Gui {
public:
    GuiLog() { }

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("sys-patch", VERSION_WITH_HASH);
        auto list = new tsl::elm::List();

        if (g_fs_helper.does_file_exist(LOG_PATH)) {
            struct CallbackUser {
                tsl::elm::List* list;
                std::string last_section;
            } callback_userdata{list};

            ini_browse([](const mTCHAR *Section, const mTCHAR *Key, const mTCHAR *Value, void *UserData){
                auto user = (CallbackUser*)UserData;
                std::string_view value{Value};
                const auto [status, detail] = split_log_value(value);

                if (status == "Skipped") {
                    return 1;
                }

                if (user->last_section != Section) {
                    user->last_section = Section;
                    user->list->addItem(new tsl::elm::CategoryHeader("Log: " + user->last_section));
                }

                if (status.starts_with("Patched")) {
                    const auto is_sys_patch = status.ends_with("(sys-patch)");
                    const auto display_value = detail.empty() ? std::string{"Patched"} : "Patched @ " + detail;
                    user->list->addItem(new ColouredListItem(
                        Key,
                        display_value,
                        is_sys_patch ? colors::syspatch : colors::file
                    ));
                } else if (status.starts_with("Unpatched") || status.starts_with("Disabled")) {
                    user->list->addItem(new ColouredListItem(Key, Value, colors::unpatched));
                } else if (user->last_section == "stats") {
                    user->list->addItem(new ColouredListItem(Key, Value, tsl::style::color::ColorDescription));
                } else {
                    user->list->addItem(new ColouredListItem(Key, Value, tsl::style::color::ColorText));
                }

                return 1;
            }, &callback_userdata, LOG_PATH);
        } else {
            list->addItem(new tsl::elm::ListItem("No log found!"));
        }

        frame->setContent(list);
        return frame;
    }
};

// Optimized: Navigation helper using lambda to avoid code duplication
auto make_nav_listener(auto target_gui) {
    return [](u64 keys) -> bool {
        if (keys & HidNpadButton_A) {
            tsl::changeTo(target_gui);
            return true;
        }
        return false;
    };
}

class GuiMain final : public tsl::Gui {
private:
    AllPatchConfigs m_config;

public:
    GuiMain() { }

    tsl::elm::Element* createUI() override {
        auto frame = new tsl::elm::OverlayFrame("sys-patch", VERSION_WITH_HASH);
        auto list = new tsl::elm::List();

        // Create menu items
        auto options = new tsl::elm::ListItem("Options");
        auto toggle = new tsl::elm::ListItem("Toggle patches");
        auto log = new tsl::elm::ListItem("Log");

        // Set click listeners with captured config reference
        options->setClickListener([this](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GuiOptions>(this->m_config);
                return true;
            }
            return false;
        });

        toggle->setClickListener([this](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GuiToggle>(this->m_config);
                return true;
            }
            return false;
        });

        log->setClickListener([](u64 keys) -> bool {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<GuiLog>();
                return true;
            }
            return false;
        });

        list->addItem(new tsl::elm::CategoryHeader("Menu"));
        list->addItem(options);
        list->addItem(toggle);
        list->addItem(log);

        frame->setContent(list);
        return frame;
    }
};

// libtesla already initialized fs, hid, pl, pmdmnt, hid:sys and set:sys
class SysPatchOverlay final : public tsl::Overlay {
public:
    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<GuiMain>();
    }
};

} // namespace

int main(int argc, char **argv) {
    g_fs_helper.create_dir("/config/");
    g_fs_helper.create_dir("/config/sys-patch/");
    return tsl::loop<SysPatchOverlay>(argc, argv);
}
