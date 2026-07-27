#include "core/AppModel.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "neogff_icon.xpm"
#include "TabularData.hpp"
#include "TslPatcher.hpp"
#include "core/GffJson.hpp"

#include <wx/aui/auibook.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/grid.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/sizer.h>
#include <wx/treectrl.h>
#include <wx/wx.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neogff;

constexpr const char* kAppName = "NeoGFF";

std::string extensionPatterns(const std::vector<std::string>& extensions) {
    std::string patterns;
    for (const std::string& extension : extensions) {
        if (!patterns.empty()) patterns += ';';
        patterns += "*." + extension;
        std::string upper = extension;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) {
            return static_cast<char>(std::toupper(ch));
        });
        if (upper != extension) patterns += ";*." + upper;
    }
    return patterns;
}

const std::string& gffWildcard() {
    static const std::string wildcard = [] {
        std::vector<std::string> allExtensions = knownGffResourceExtensions();
        std::vector<std::string> additionalGffFormats =
            dragonAgeGff4ResourceExtensions();
        additionalGffFormats.push_back("gda");
        for (const std::string& extension : additionalGffFormats) {
            if (std::find(allExtensions.begin(), allExtensions.end(), extension) ==
                allExtensions.end()) {
                allExtensions.push_back(extension);
            }
        }

        const std::string allPatterns =
            extensionPatterns(allExtensions) + ";*.#*;hash_*.*";
        const std::string jadePatterns =
            extensionPatterns(jadeEmpireGffResourceExtensions());
        const std::string dragonAgePatterns =
            extensionPatterns(dragonAgeGff4ResourceExtensions()) + ";hash_*.*";

        return "GFF-backed files|" + allPatterns +
               "|Jade Empire GFF resources|" + jadePatterns +
               "|Dragon Age GDA/G2DA files|*.gda;*.GDA" +
               "|Dragon Age GFF4 files|" + dragonAgePatterns +
               "|All files (*.*)|*.*";
    }();
    return wildcard;
}
constexpr const char* kTlkWildcard = "TLK files (*.tlk)|*.tlk|All files (*.*)|*.*";
constexpr const char* kXmlTableWildcard = "XML files (*.xml)|*.xml|All files (*.*)|*.*";
constexpr const char* kJsonTableWildcard = "JSON files (*.json)|*.json|All files (*.*)|*.*";

const char* tableWildcardForFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Xml: return kXmlTableWildcard;
    case neotabular::Format::Json: return kJsonTableWildcard;
    default: throw std::invalid_argument("NeoGFF supports semantic XML and JSON table import/export only.");
    }
}

std::string exportExtensionForFormat(neotabular::Format format) {
    switch (format) {
    case neotabular::Format::Xml: return "xml";
    case neotabular::Format::Json: return "json";
    default: throw std::invalid_argument("NeoGFF supports semantic XML and JSON table import/export only.");
    }
}

std::string exportDefaultFilename(const std::filesystem::path& source,
                                  neotabular::Format format,
                                  const std::string& fallbackStem) {
    std::string stem = source.empty() ? fallbackStem : source.stem().string();
    if (stem.empty()) stem = fallbackStem.empty() ? std::string("export") : fallbackStem;
    return stem + "." + exportExtensionForFormat(format);
}

std::string normalizedGffPatcherType(std::string type) {
    type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), type.end());
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return type;
}

void requireGenericGffPatcherModel(const GffModel& model, const std::string& role) {
    if (!model.loaded()) throw std::runtime_error(role + " is not loaded.");
    if (model.gff().isGff4()) {
        throw std::runtime_error(
            role + " is GFF4. TSLPatcher/HoloPatcher [GFFList] output supports GFF3 files only.");
    }
    if (normalizedGffPatcherType(model.fileType()) == "DLG") {
        throw std::runtime_error(
            role + " is a DLG file. Open it in NeoDLG and use the DLG-aware patcher exporter so dialogue indexes remain dynamic.");
    }
}

void requireMatchingGffPatcherModels(const GffModel& original, const GffModel& modified) {
    requireGenericGffPatcherModel(original, "The original patch baseline");
    requireGenericGffPatcherModel(modified, "The modified patch document");
    if (normalizedGffPatcherType(original.fileType()) != normalizedGffPatcherType(modified.fileType())) {
        throw std::runtime_error("The original and modified GFF file types do not match.");
    }
    if (original.version() != modified.version()) {
        throw std::runtime_error("The original and modified GFF versions do not match.");
    }
}

constexpr int kColPath = 0;
constexpr int kColLabel = 1;
constexpr int kColType = 2;
constexpr int kColEditable = 3;
constexpr int kColValue = 4;
constexpr int kColResolved = 5;
constexpr int kColumnCount = 6;

std::string gffColumnLabel(std::size_t column) {
    static const std::vector<std::string> labels = {"Path", "Label", "Type", "Editable", "Value", "Resolved"};
    return column < labels.size() ? labels[column] : ("Column " + std::to_string(column));
}

enum : int {
    ID_New = wxID_HIGHEST + 1,
    ID_Open,
    ID_Save,
    ID_SaveAs,
    ID_CloseTab,
    ID_CloseOtherTabs,
    ID_NextTab,
    ID_PreviousTab,
    ID_DocumentTabs,
    ID_OpenTlk,
    ID_ClearTlk,
    ID_AddField,
    ID_DeleteField,
    ID_CopyCells,
    ID_PasteCells,
    ID_Filter,
    ID_ClearFilter,
    ID_FilterColumn,
    ID_ClearColumnFilter,
    ID_ClearAllFilters,
    ID_MoveColumnLeft,
    ID_MoveColumnRight,
    ID_ResetColumnOrder,
    ID_ResetRowOrder,
    ID_ImportXml,
    ID_ImportJson,
    ID_ExportXml,
    ID_ExportJson,
    ID_ExportPatcherPackage,
    ID_ExportPatcherFragment,
    ID_ViewFlatGrid,
    ID_ViewElementTree,
    ID_ExpandTree,
    ID_CollapseTree,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_Grid,
    ID_ElementTree
};

constexpr int kRecentFileBaseId = wxID_HIGHEST + 1000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

enum class GffViewMode {
    FlatGrid,
    ElementTree
};

std::string viewModeConfigValue(GffViewMode mode) {
    return mode == GffViewMode::ElementTree ? std::string("tree") : std::string("grid");
}

GffViewMode readPreferredViewMode() {
    const std::string text = neosettings::AppSettings(kAppName).preferredView();
    if (text == "tree" || text == "element-tree" || text == "ElementTree") {
        return GffViewMode::ElementTree;
    }
    return GffViewMode::FlatGrid;
}

void writePreferredViewMode(GffViewMode mode) {
    neosettings::AppSettings(kAppName).setPreferredView(viewModeConfigValue(mode));
}

bool isTreeView(GffViewMode mode) {
    return mode == GffViewMode::ElementTree;
}

std::string pathText(const std::filesystem::path& path) {
    return path.empty() ? std::string{} : path.string();
}

std::string parentPathOf(std::string path) {
    const auto suffix = path.find('(');
    if (suffix != std::string::npos) path = path.substr(0, suffix);
    const auto pos = path.find_last_of('\\');
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

std::string treeParentPathOf(std::string path) {
    const auto suffix = path.find('(');
    if (suffix != std::string::npos) return path.substr(0, suffix);
    const auto pos = path.find_last_of('\\');
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

std::string pathLeaf(std::string path) {
    const auto suffix = path.find('(');
    if (suffix != std::string::npos) path = path.substr(0, suffix);
    const auto pos = path.find_last_of('\\');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string ellipsize(std::string text, std::size_t maxChars) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() <= maxChars) return text;
    if (maxChars <= 3) return text.substr(0, maxChars);
    text.resize(maxChars - 3);
    text += "...";
    return text;
}

std::string treeTextForRow(const GffFieldRow& row) {
    std::string name = row.label.empty() || row.label == "(empty)" ? pathLeaf(row.path) : row.label;
    if (name.empty()) name = row.path.empty() ? std::string("Main Struct") : row.path;

    std::string text = name;
    if (!row.type.empty()) text += " [" + row.type + "]";
    if (!row.value.empty()) text += " " + ellipsize(row.value, 96);
    if (!row.resolved.empty()) text += " -> " + ellipsize(row.resolved, 96);
    return text;
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open input text file: " + file.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Unable to open output text file: " + file.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("Unable to write output text file: " + file.string());
}

std::string trimFixedHeaderText(std::string text) {
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

std::string typeVersionText(const GffModel& model) {
    if (!model.loaded()) return {};
    const std::string type = trimFixedHeaderText(model.fileType());
    const std::string version = trimFixedHeaderText(model.version());
    if (type.empty()) return version;
    if (version.empty()) return type;
    return type + " " + version;
}

std::optional<std::filesystem::path> readCachedTlkPath() {
    return neosettings::AppSettings(kAppName).lastTlkPath();
}

void writeCachedTlkPath(const std::filesystem::path& path) {
    neosettings::AppSettings(kAppName).setLastTlkPath(path);
}

void clearCachedTlkPath() {
    neosettings::AppSettings(kAppName).clearLastTlkPath();
}


class GffTreeItemData final : public wxTreeItemData {
public:
    GffTreeItemData(std::string itemPath, int rowIndex)
        : path_(std::move(itemPath)), rowIndex_(rowIndex) {}

    const std::string& path() const noexcept { return path_; }
    int rowIndex() const noexcept { return rowIndex_; }
    void setRowIndex(int rowIndex) noexcept { rowIndex_ = rowIndex; }

private:
    std::string path_;
    int rowIndex_ = -1;
};

class AddFieldDialog final : public wxDialog {
public:
    AddFieldDialog(wxWindow* parent, const std::string& initialParent)
        : wxDialog(parent, wxID_ANY, "Add GFF Field", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);

        form->Add(new wxStaticText(this, wxID_ANY, "Parent path:"), 0, wxALIGN_CENTER_VERTICAL);
        parentPath_ = new wxTextCtrl(this, wxID_ANY, wxui::toWx(initialParent));
        form->Add(parentPath_, 1, wxEXPAND);

        form->Add(new wxStaticText(this, wxID_ANY, "Label:"), 0, wxALIGN_CENTER_VERTICAL);
        label_ = new wxTextCtrl(this, wxID_ANY);
        label_->SetMaxLength(16);
        form->Add(label_, 1, wxEXPAND);

        form->Add(new wxStaticText(this, wxID_ANY, "Type:"), 0, wxALIGN_CENTER_VERTICAL);
        type_ = new wxChoice(this, wxID_ANY);
        for (const auto& typeName : supportedFieldTypeNames()) {
            type_->Append(wxui::toWx(typeName));
        }
        type_->SetStringSelection("CExoString");
        form->Add(type_, 1, wxEXPAND);

        form->Add(new wxStaticText(this, wxID_ANY, "Value:"), 0, wxALIGN_CENTER_VERTICAL);
        value_ = new wxTextCtrl(this, wxID_ANY);
        form->Add(value_, 1, wxEXPAND);

        form->Add(new wxStaticText(this, wxID_ANY, "Struct Type ID:"), 0, wxALIGN_CENTER_VERTICAL);
        structTypeId_ = new wxTextCtrl(this, wxID_ANY, "0");
        form->Add(structTypeId_, 1, wxEXPAND);

        root->Add(form, 1, wxEXPAND | wxALL, 12);
        auto* buttons = CreateSeparatedButtonSizer(wxOK | wxCANCEL);
        if (buttons) root->Add(buttons, 0, wxEXPAND | wxALL, 12);
        SetSizer(root);
        SetMinSize(FromDIP(wxSize(520, 260)));
        SetInitialSize(FromDIP(wxSize(620, 320)));
    }

    std::string parentPath() const { return wxui::toStd(parentPath_->GetValue()); }
    std::string label() const { return wxui::toStd(label_->GetValue()); }
    std::string type() const { return wxui::toStd(type_->GetStringSelection()); }
    std::string value() const { return wxui::toStd(value_->GetValue()); }
    std::uint32_t structTypeId() const { return ParseUInt32Decimal(wxui::toStd(structTypeId_->GetValue())); }

private:
    wxTextCtrl* parentPath_ = nullptr;
    wxTextCtrl* label_ = nullptr;
    wxChoice* type_ = nullptr;
    wxTextCtrl* value_ = nullptr;
    wxTextCtrl* structTypeId_ = nullptr;
};

class NeoGFFFrame final : public wxFrame {
public:
    NeoGFFFrame()
        : wxFrame(nullptr, wxID_ANY, "NeoGFF v1.0.0 (GFF editor)", wxDefaultPosition, wxDefaultSize) {
        pendingViewMode_ = readPreferredViewMode();
        setApplicationIcon();
        buildMenus();
        buildWindow();
        wxui::createStatusBar(*this, 2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        createDocumentTab(true);
        tryLoadCachedTlk();
        SetMinSize(FromDIP(wxSize(860, 560)));
        SetInitialSize(FromDIP(wxSize(1120, 760)));
        settings_.restoreWindowPlacement(*this);
        refreshAll();
    }

    void openStartupFile(const std::filesystem::path& path) {
        if (path.empty()) return;
        try {
            openModelPath(path, false);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

private:

    struct DocumentTab {
        std::unique_ptr<GffModel> model = std::make_unique<GffModel>();
        neoview::DocumentViewState viewState;
        GffViewMode viewMode = GffViewMode::FlatGrid;
        std::string tlkAutoLoadWarning;
        std::string untitledName = "Untitled GFF";
        wxWindow* tabPage = nullptr;
    };

    bool hasActiveDocument() const {
        return activeDocumentIndex_ != neotabs::npos && activeDocumentIndex_ < documents_.size();
    }

    DocumentTab& activeDocument() { return documents_.at(activeDocumentIndex_); }
    const DocumentTab& activeDocument() const { return documents_.at(activeDocumentIndex_); }
    GffModel& model() { return *activeDocument().model; }
    const GffModel& model() const { return *activeDocument().model; }
    neoview::DocumentViewState& viewState() { return activeDocument().viewState; }
    const neoview::DocumentViewState& viewState() const { return activeDocument().viewState; }
    GffViewMode& viewMode() { return hasActiveDocument() ? activeDocument().viewMode : pendingViewMode_; }
    const GffViewMode& viewMode() const { return hasActiveDocument() ? activeDocument().viewMode : pendingViewMode_; }
    std::string& tlkAutoLoadWarning() { return activeDocument().tlkAutoLoadWarning; }
    const std::string& tlkAutoLoadWarning() const { return activeDocument().tlkAutoLoadWarning; }

    bool tabDirty(const DocumentTab& tab) const { return tab.model && tab.model->dirty(); }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(tab.model ? tab.model->filename() : std::filesystem::path{}, tab.untitledName);
    }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        neotabs::setTabLabel(documentTabs_, activeDocument().tabPage, tabDisplayName(activeDocument()), tabDirty(activeDocument()));
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        setViewMode(viewMode(), false);
        refreshAll();
    }

    void createDocumentTab(bool select = true) {
        DocumentTab tab;
        tab.model = std::make_unique<GffModel>();
        tab.viewMode = pendingViewMode_;
        tab.viewState.resetForNewDocument();
        tab.viewState.preferredViewMode = tab.viewMode == GffViewMode::ElementTree ? "ElementTree" : "FlatGrid";
        tab.viewState.selectedLogicalRow = -1;
        const std::size_t previousActiveIndex = activeDocumentIndex_;
        documents_.push_back(std::move(tab));
        const std::size_t index = documents_.size() - 1;

        tabSwitchInProgress_ = true;
        wxWindow* const page = neotabs::addTabPage(
            documentTabs_, tabDisplayName(documents_.back()), tabDirty(documents_.back()), select);
        if (page != nullptr) documents_.back().tabPage = page;
        tabSwitchInProgress_ = false;

        if (page == nullptr) {
            documents_.pop_back();
            activeDocumentIndex_ = previousActiveIndex;
            throw std::runtime_error("Unable to create a document tab.");
        }

        if (select) {
            activeDocumentIndex_ = index;
            tabSwitchInProgress_ = true;
            neotabs::changeSelectionToPage(documentTabs_, page);
            tabSwitchInProgress_ = false;
            setViewMode(viewMode(), false);
            refreshAll();
        }
    }

    bool activeTabIsReusableForOpen() const {
        return hasActiveDocument() && documents_.size() == 1 && !tabDirty(activeDocument()) && !model().loaded();
    }

    void ensureDocumentTabForOpen() {
        if (!hasActiveDocument()) { createDocumentTab(true); return; }
        if (!activeTabIsReusableForOpen()) createDocumentTab(true);
    }

    bool confirmCloseDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocumentTab(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocumentTab(index)) return false;

        wxWindow* const page = documents_[index].tabPage;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;

        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(true);
            return true;
        }

        std::size_t selectedIndex = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (selectedIndex == neotabs::npos) selectedIndex = std::min(index, documents_.size() - 1);
        selectDocumentTab(selectedIndex);
        return true;
    }

    bool confirmCloseAllTabs() {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!confirmCloseDocumentTab(i)) return false;
        }
        return true;
    }

    void onDocumentTabChanged(wxAuiNotebookEvent& event) {
        if (tabSwitchInProgress_) { event.Skip(); return; }
        const int selection = event.GetSelection();
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) selectDocumentTab(index);
        event.Skip();
    }

    void onDocumentTabCloseRequested(wxAuiNotebookEvent& event) {
        event.Veto();
        const int selection = event.GetSelection();
        if (selection < 0) return;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, selection));
        if (index != neotabs::npos) closeDocumentTab(index);
    }

    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neogff", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) bundle.AddIcon(windowsIcon);
#endif
        wxIcon fallbackIcon(neogff_icon_xpm);
        if (fallbackIcon.IsOk()) bundle.AddIcon(fallbackIcon);
        if (bundle.GetIconCount() > 0) SetIcons(bundle);
    }

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;

    void buildMenus() {
        auto* file = new wxMenu;
        file->Append(ID_New, "&New GFF...");
        file->Append(ID_Open, "&Open GFF Resource...");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->Append(ID_OpenTlk, "Open optional &TLK...");
        file->Append(ID_ClearTlk, "Clear TLK");
        file->Append(ID_Save, "&Save");
        file->Append(ID_SaveAs, "Save &As...");
        file->AppendSeparator();
        file->Append(ID_CloseTab, "&Close Tab\tCtrl-W");
        file->Append(ID_CloseOtherTabs, "Close &Other Tabs");
        file->Append(ID_NextTab, "Next Tab\tCtrl-Tab");
        file->Append(ID_PreviousTab, "Previous Tab\tCtrl-Shift-Tab");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) {
                chooseAndOpenGff(directory);
            });
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit");

        auto* import = new wxMenu;
        import->Append(ID_ImportXml, "Import &XML...");
        import->Append(ID_ImportJson, "Import &JSON...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportXml, "Export as &XML...");
        exportMenu->Append(ID_ExportJson, "Export as &JSON...");
        exportMenu->AppendSeparator();
        exportMenu->Append(ID_ExportPatcherPackage, "Export TSL/HoloPatcher &Package...");
        exportMenu->Append(ID_ExportPatcherFragment, "Export TSL/HoloPatcher &Fragment...");

        auto* edit = new wxMenu;
        edit->Append(ID_CopyCells, "&Copy Cells	Ctrl-C");
        edit->Append(ID_PasteCells, "&Paste Cells	Ctrl-V");
        edit->AppendSeparator();
        edit->Append(ID_Filter, "&Filter/Search...	Ctrl-F");
        edit->Append(ID_FilterColumn, "Filter Selected &Column...");
        edit->Append(ID_ClearColumnFilter, "Clear Filter on Selected Column");
        edit->Append(ID_ClearAllFilters, "Clear &All Filters");
        edit->AppendSeparator();
        edit->Append(ID_AddField, "&Add Field...");
        edit->Append(ID_DeleteField, "&Delete Selected Field");

        auto* view = new wxMenu;
        flatGridViewItem_ = view->AppendRadioItem(ID_ViewFlatGrid, "Flat &Grid View");
        elementTreeViewItem_ = view->AppendRadioItem(ID_ViewElementTree, "&Element Tree View");
        view->AppendSeparator();
        view->Append(ID_ExpandTree, "E&xpand Element Tree");
        view->Append(ID_CollapseTree, "&Collapse Element Tree");
        view->AppendSeparator();
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Font Size\tCtrl+0");
        view->AppendSeparator();
        view->Append(ID_MoveColumnLeft, "Move Selected Column Left");
        view->Append(ID_MoveColumnRight, "Move Selected Column Right");
        view->Append(ID_ResetColumnOrder, "Reset Column Order");
        view->Append(ID_ResetRowOrder, "Reset Row Order");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(import, "&Import");
        bar->Append(exportMenu, "&Export");
        bar->Append(edit, "&Edit");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);

        Bind(wxEVT_MENU, &NeoGFFFrame::onNew, this, ID_New);
        Bind(wxEVT_MENU, &NeoGFFFrame::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoGFFFrame::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &NeoGFFFrame::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoGFFFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoGFFFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoGFFFrame::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoGFFFrame::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoGFFFrame::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoGFFFrame::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoGFFFrame::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_MENU, &NeoGFFFrame::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_MENU, &NeoGFFFrame::onAddField, this, ID_AddField);
        Bind(wxEVT_MENU, &NeoGFFFrame::onDeleteField, this, ID_DeleteField);
        Bind(wxEVT_MENU, &NeoGFFFrame::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &NeoGFFFrame::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, &NeoGFFFrame::onFilterPrompt, this, ID_Filter);
        Bind(wxEVT_MENU, &NeoGFFFrame::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, &NeoGFFFrame::onFilterSelectedColumn, this, ID_FilterColumn);
        Bind(wxEVT_MENU, &NeoGFFFrame::onClearSelectedColumnFilter, this, ID_ClearColumnFilter);
        Bind(wxEVT_MENU, &NeoGFFFrame::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, &NeoGFFFrame::onMoveColumnLeft, this, ID_MoveColumnLeft);
        Bind(wxEVT_MENU, &NeoGFFFrame::onMoveColumnRight, this, ID_MoveColumnRight);
        Bind(wxEVT_MENU, &NeoGFFFrame::onResetColumnOrder, this, ID_ResetColumnOrder);
        Bind(wxEVT_MENU, &NeoGFFFrame::onResetRowOrder, this, ID_ResetRowOrder);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Xml); }, ID_ImportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Json); }, ID_ImportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Xml); }, ID_ExportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Json); }, ID_ExportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExportPatcher(true); }, ID_ExportPatcherPackage);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExportPatcher(false); }, ID_ExportPatcherFragment);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setViewMode(GffViewMode::FlatGrid, true); }, ID_ViewFlatGrid);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setViewMode(GffViewMode::ElementTree, true); }, ID_ViewElementTree);
        Bind(wxEVT_MENU, &NeoGFFFrame::onExpandTree, this, ID_ExpandTree);
        Bind(wxEVT_MENU, &NeoGFFFrame::onCollapseTree, this, ID_CollapseTree);
        Bind(wxEVT_MENU, &NeoGFFFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoGFFFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoGFFFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoGFFFrame::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoGFF", "NeoGFF v1.0.0\nNative wxWidgets BioWare GFF editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, wxID_ABOUT);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onNew, this, ID_New);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onAddField, this, ID_AddField);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onDeleteField, this, ID_DeleteField);
        Bind(wxEVT_BUTTON, &NeoGFFFrame::onClearFilter, this, ID_ClearFilter);
    }

    void buildWindow() {
        auto* panel = new wxPanel(this);
        auto* root = new wxBoxSizer(wxVERTICAL);

        documentTabs_ = new wxAuiNotebook(panel, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB | wxAUI_NB_SCROLL_BUTTONS);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        neotabs::configureDocumentTabStrip(documentTabs_);

        auto* fileBox = new wxStaticBoxSizer(wxVERTICAL, panel, "GFF");
        auto* header = new wxBoxSizer(wxVERTICAL);

        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(panel, wxID_ANY, "File:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        filePath_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        fileRow->Add(filePath_, 1, wxEXPAND | wxRIGHT, 8);
        fileRow->Add(new wxButton(panel, ID_New, "New"), 0, wxRIGHT, 4);
        fileRow->Add(new wxButton(panel, ID_Open, "Open..."), 0, wxRIGHT, 4);
        fileRow->Add(new wxButton(panel, ID_Save, "Save"), 0, wxRIGHT, 4);
        fileRow->Add(new wxButton(panel, ID_SaveAs, "Save As..."), 0);
        header->Add(fileRow, 0, wxEXPAND | wxBOTTOM, 6);

        auto* infoRow = new wxBoxSizer(wxHORIZONTAL);
        infoRow->Add(new wxStaticText(panel, wxID_ANY, "Type/version:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        typeText_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        typeText_->SetMinSize(FromDIP(wxSize(180, -1)));
        infoRow->Add(typeText_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
        infoRow->AddStretchSpacer(1);
        infoRow->Add(new wxButton(panel, ID_AddField, "Add Field..."), 0, wxRIGHT, 4);
        infoRow->Add(new wxButton(panel, ID_DeleteField, "Delete Selected"), 0);
        header->Add(infoRow, 0, wxEXPAND | wxBOTTOM, 6);

        auto* tlkRow = new wxBoxSizer(wxHORIZONTAL);
        tlkRow->Add(new wxStaticText(panel, wxID_ANY, "TLK:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        tlkPath_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        tlkRow->Add(tlkPath_, 1, wxEXPAND | wxRIGHT, 8);
        tlkRow->Add(new wxButton(panel, ID_OpenTlk, "Open optional TLK..."), 0, wxRIGHT, 4);
        tlkRow->Add(new wxButton(panel, ID_ClearTlk, "Clear TLK"), 0);
        header->Add(tlkRow, 0, wxEXPAND | wxBOTTOM, 6);

        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(panel, wxID_ANY, "Filter:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        filterText_ = new wxTextCtrl(panel, wxID_ANY);
        filterRow->Add(filterText_, 1, wxEXPAND | wxRIGHT, 4);
        filterRow->Add(new wxButton(panel, ID_ClearFilter, "Clear"), 0);
        header->Add(filterRow, 0, wxEXPAND);

        fileBox->Add(header, 0, wxEXPAND | wxALL, 8);
        root->Add(fileBox, 0, wxEXPAND | wxALL, 8);

        viewPanel_ = new wxPanel(panel);
        viewSizer_ = new wxBoxSizer(wxVERTICAL);

        grid_ = new wxGrid(viewPanel_, ID_Grid);
        grid_->CreateGrid(0, kColumnCount);
        wxui::configureStableGridRendering(*grid_);
        grid_->SetColLabelValue(kColPath, "Path");
        grid_->SetColLabelValue(kColLabel, "Label");
        grid_->SetColLabelValue(kColType, "Type");
        grid_->SetColLabelValue(kColEditable, "Editable");
        grid_->SetColLabelValue(kColValue, "Value");
        grid_->SetColLabelValue(kColResolved, "Resolved");
        grid_->EnableEditing(true);
        grid_->SetColSize(kColPath, FromDIP(300));
        grid_->SetColSize(kColLabel, FromDIP(160));
        grid_->SetColSize(kColType, FromDIP(140));
        grid_->SetColSize(kColEditable, FromDIP(80));
        grid_->SetColSize(kColValue, FromDIP(300));
        grid_->SetColSize(kColResolved, FromDIP(380));

        tree_ = new wxTreeCtrl(viewPanel_, ID_ElementTree, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);

        viewSizer_->Add(grid_, 1, wxEXPAND);
        viewSizer_->Add(tree_, 1, wxEXPAND);
        viewPanel_->SetSizer(viewSizer_);
        root->Add(viewPanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);
        setViewMode(pendingViewMode_, false);
        if (filterText_) filterText_->Bind(wxEVT_TEXT, &NeoGFFFrame::onFilterText, this);
        Bind(wxEVT_GRID_CELL_CHANGED, &NeoGFFFrame::onCellChanged, this, ID_Grid);
        Bind(wxEVT_GRID_LABEL_RIGHT_CLICK, &NeoGFFFrame::onGridLabelRightClick, this, ID_Grid);
        Bind(wxEVT_TREE_SEL_CHANGED, &NeoGFFFrame::onTreeSelectionChanged, this, ID_ElementTree);
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoGFFFrame::onTreeActivated, this, ID_ElementTree);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoGFFFrame::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoGFFFrame::onDocumentTabCloseRequested, this);
        Bind(wxEVT_CLOSE_WINDOW, &NeoGFFFrame::onClose, this);
    }

    void rebuildRecentFilesMenu() {
        if (recentFilesMenu_ != nullptr) {
            neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, kRecentFileBaseId, kClearRecentFilesId);
        }
    }

    void rememberRecentFile(const std::filesystem::path& path) {
        settings_.addRecentFile(path);
        rebuildRecentFilesMenu();
    }

    void tryLoadResolvedTlkForPath(const std::filesystem::path& path) {
        if (model().tlk().loaded()) return;
        const auto tlk = neogames::resolver().bestTlkForPath(path);
        if (!tlk || tlk->empty()) return;
        try {
            model().loadTlk(*tlk);
            writeCachedTlkPath(*tlk);
            tlkAutoLoadWarning().clear();
        } catch (const std::exception& ex) {
            tlkAutoLoadWarning() = std::string("Unable to auto-load resolved TLK: ") + ex.what();
        }
    }

    bool openModelPath(const std::filesystem::path& path, bool checkDirty = true) {
        if (path.empty()) return false;
        (void)checkDirty;
        ensureDocumentTabForOpen();
        model().load(path);
        const std::string preferredView = viewState().preferredViewMode;
        viewState().resetForNewDocument();
        viewState().preferredViewMode = preferredView;
        viewState().selectedLogicalRow = -1;
        setFilterTerm({});
        tryLoadResolvedTlkForPath(path);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        refreshAll();
        return true;
    }

    void onOpenRecent(wxCommandEvent& event) {
        const int index = event.GetId() - kRecentFileBaseId;
        const auto files = settings_.recentFiles();
        if (index < 0 || static_cast<std::size_t>(index) >= files.size()) return;
        try {
            if (!std::filesystem::exists(files[static_cast<std::size_t>(index)])) {
                settings_.removeRecentFile(files[static_cast<std::size_t>(index)]);
                rebuildRecentFilesMenu();
                throw std::runtime_error("Recent file no longer exists: " + files[static_cast<std::size_t>(index)].string());
            }
            openModelPath(files[static_cast<std::size_t>(index)], true);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onClearRecentFiles(wxCommandEvent&) {
        settings_.clearRecentFiles();
        rebuildRecentFilesMenu();
    }

    bool maybeSave() {
        if (!model().loaded() || !model().dirty()) return true;
        const int result = wxMessageBox("The current GFF has unsaved changes. Save it?", "Unsaved Changes",
                                        wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (result == wxCANCEL) return false;
        if (result == wxYES) return save(false);
        return true;
    }

    void onNew(wxCommandEvent&) {
        auto type = wxui::promptText(this, "New GFF", "GFF file type:", "UTC");
        if (!type) return;
        try {
            createDocumentTab(true);
            model().newFile(*type);
            const std::string preferredView = viewState().preferredViewMode;
            viewState().resetForNewDocument();
            viewState().preferredViewMode = preferredView;
            viewState().selectedLogicalRow = -1;
            setFilterTerm({});
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpenGff(const std::filesystem::path& initialDirectory = {}) {
        auto file = wxui::chooseOpenFile(this, "Open GFF-backed resource", gffWildcard(), initialDirectory);
        if (!file) return;
        try {
            openModelPath(*file, false);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onOpen(wxCommandEvent&) {
        chooseAndOpenGff();
    }

    void onOpenTlk(wxCommandEvent&) {
        auto file = wxui::chooseOpenFile(this, "Open optional TLK for resolved StrRef text", kTlkWildcard);
        if (!file) return;
        try {
            model().loadTlk(*file);
            writeCachedTlkPath(*file);
            tlkAutoLoadWarning().clear();
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onClearTlk(wxCommandEvent&) {
        model().clearTlk();
        clearCachedTlkPath();
        tlkAutoLoadWarning().clear();
        refreshAll();
    }

    void tryLoadCachedTlk() {
        const auto cached = readCachedTlkPath();
        if (!cached || cached->empty()) return;
        try {
            if (!std::filesystem::exists(*cached)) {
                tlkAutoLoadWarning() = "Cached TLK not found: " + cached->string();
                return;
            }
            model().loadTlk(*cached);
            tlkAutoLoadWarning().clear();
        } catch (const std::exception& ex) {
            tlkAutoLoadWarning() = std::string("Unable to auto-load cached TLK: ") + ex.what();
        }
    }

    void onSave(wxCommandEvent&) { (void)save(false); }
    void onSaveAs(wxCommandEvent&) { (void)save(true); }

    void setFilterTerm(std::string term) {
        viewState().filterTerm = std::move(term);
        if (filterText_ != nullptr && wxui::toStd(filterText_->GetValue()) != viewState().filterTerm) {
            filterText_->ChangeValue(wxui::toWx(viewState().filterTerm));
        }
        refreshAll();
    }

    void onFilterText(wxCommandEvent&) {
        viewState().filterTerm = filterText_ ? wxui::toStd(filterText_->GetValue()) : std::string();
        refreshAll();
    }

    void onFilterPrompt(wxCommandEvent&) {
        auto term = wxui::promptText(this, "Filter/Search", "Search term:", viewState().filterTerm);
        if (term) setFilterTerm(*term);
    }

    void clearAllFiltersAndRefresh() {
        neoview::clearAllFilters(viewState());
        if (filterText_ != nullptr && !filterText_->GetValue().empty()) filterText_->ChangeValue(wxString{});
        refreshAll();
    }

    std::size_t actualColumnForVisible(int visualColumn) const {
        try {
            return neoview::logicalColumnForVisual(viewState(), visualColumn);
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Selected column is outside the current view.");
        }
    }

    int selectedVisualColumn() const {
        if (contextVisualColumn_ >= 0) return contextVisualColumn_;
        return grid_ ? grid_->GetGridCursorCol() : -1;
    }

    void promptColumnFilterForVisualColumn(int visualColumn) {
        const std::size_t logicalColumn = actualColumnForVisible(visualColumn);
        const auto* existing = neoview::findColumnFilter(viewState(), logicalColumn);
        const std::string label = gffColumnLabel(logicalColumn);
        const auto term = wxui::promptText(this, "Filter Column", "Show rows where column '" + label + "' contains:", existing ? existing->term : std::string());
        if (!term) return;
        neoview::setColumnFilter(viewState(), neoview::ColumnFilter{logicalColumn, label, *term, neoview::TextFilterMode::Contains, true});
        refreshAll();
    }

    void onClearFilter(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
    }

    void onFilterSelectedColumn(wxCommandEvent&) {
        try {
            if (isTreeView(viewMode())) {
                throw std::runtime_error("Column filters apply to the flat grid view. Switch to Flat Grid View first.");
            }
            promptColumnFilterForVisualColumn(selectedVisualColumn());
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
        contextVisualColumn_ = -1;
    }

    void onClearSelectedColumnFilter(wxCommandEvent&) {
        try {
            neoview::clearColumnFilter(viewState(), actualColumnForVisible(selectedVisualColumn()));
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
        contextVisualColumn_ = -1;
    }

    void onClearAllFilters(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
    }

    void onMoveColumnLeft(wxCommandEvent&) {
        const int visual = selectedVisualColumn();
        if (neoview::moveVisualColumn(viewState(), visual, visual - 1)) refreshAll();
    }

    void onMoveColumnRight(wxCommandEvent&) {
        const int visual = selectedVisualColumn();
        if (neoview::moveVisualColumn(viewState(), visual, visual + 1)) refreshAll();
    }

    void onResetColumnOrder(wxCommandEvent&) {
        neoview::setIdentityColumns(viewState(), kColumnCount);
        refreshAll();
    }

    void onResetRowOrder(wxCommandEvent&) {
        refreshAll();
    }

    void onGridLabelRightClick(wxGridEvent& event) {
        if (event.GetCol() >= 0) {
            contextVisualColumn_ = event.GetCol();
            wxMenu menu;
            menu.Append(ID_FilterColumn, "Filter This Column...");
            menu.Append(ID_ClearColumnFilter, "Clear Filter on This Column");
            menu.AppendSeparator();
            menu.Append(ID_MoveColumnLeft, "Move Column Left");
            menu.Append(ID_MoveColumnRight, "Move Column Right");
            menu.Append(ID_ResetColumnOrder, "Reset Column Order");
            PopupMenu(&menu);
            return;
        }
        event.Skip();
    }

    void setViewMode(GffViewMode mode, bool persist) {
        // buildWindow() selects the initial view before the first document tab
        // exists. Keep that choice in pendingViewMode_ until createDocumentTab()
        // creates a per-document view state; never index the empty document
        // vector during frame construction.
        viewMode() = mode;
        if (hasActiveDocument()) {
            viewState().preferredViewMode =
                mode == GffViewMode::ElementTree ? "ElementTree" : "FlatGrid";
        }
        if (persist) writePreferredViewMode(mode);
        if (flatGridViewItem_) flatGridViewItem_->Check(mode == GffViewMode::FlatGrid);
        if (elementTreeViewItem_) elementTreeViewItem_->Check(mode == GffViewMode::ElementTree);
        if (grid_) grid_->Show(mode == GffViewMode::FlatGrid);
        if (tree_) tree_->Show(mode == GffViewMode::ElementTree);
        if (viewSizer_) viewSizer_->Layout();
        if (viewPanel_) viewPanel_->Layout();
        Layout();
    }

    void onExpandTree(wxCommandEvent&) {
        if (!tree_) return;
        expandTreeRecursive(tree_->GetRootItem());
    }

    void onCollapseTree(wxCommandEvent&) {
        if (!tree_) return;
        const wxTreeItemId root = tree_->GetRootItem();
        collapseTreeRecursive(root);
        if (root.IsOk()) tree_->Expand(root);
    }

    void onTreeSelectionChanged(wxTreeEvent& event) {
        viewState().selectedLogicalRow = -1;
        viewState().selectedPath.clear();
        const wxTreeItemId item = event.GetItem();
        if (tree_ && item.IsOk()) {
            if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
                viewState().selectedLogicalRow = data->rowIndex();
                viewState().selectedPath = data->path();
                if (grid_ && viewState().selectedLogicalRow >= 0 && viewState().selectedLogicalRow < grid_->GetNumberRows()) {
                    grid_->SetGridCursor(viewState().selectedLogicalRow, kColValue);
                    grid_->SelectRow(viewState().selectedLogicalRow);
                    grid_->MakeCellVisible(viewState().selectedLogicalRow, kColValue);
                } else if (grid_) {
                    grid_->ClearSelection();
                }
            }
        }
        event.Skip();
    }

    void onImport(neotabular::Format format) {
        try {
            const auto chosen = wxui::chooseOpenFile(this, "Import " + neotabular::formatName(format), tableWildcardForFormat(format));
            if (!chosen) return;
            if (format == neotabular::Format::Xml) {
                model().importXml(readTextFile(*chosen));
            } else if (format == neotabular::Format::Json) {
                model().importXml(gffJsonToXml(readTextFile(*chosen)));
            } else {
                throw std::invalid_argument("NeoGFF imports only semantic XML or JSON. CSV/TSV flattened import is not supported for GFF files.");
            }
            const std::string preferredMode = viewState().preferredViewMode;
            viewState().resetForNewDocument();
            viewState().preferredViewMode = preferredMode;
            if (filterText_) filterText_->ChangeValue("");
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExport(neotabular::Format format) {
        if (!model().loaded()) return;
        try {
            const auto chosen = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), tableWildcardForFormat(format),
                                                   exportDefaultFilename(model().filename(), format, "gff"));
            if (!chosen) return;
            if (format == neotabular::Format::Xml || format == neotabular::Format::Json) {
                if (neoview::hasAnyFilter(viewState())) {
                    throw std::invalid_argument("Semantic GFF XML/JSON export preserves hierarchy and does not support row filtering.");
                }
                const std::string xml = model().toXml();
                writeTextFile(*chosen, format == neotabular::Format::Json ? gffXmlToJson(xml) : xml);
            } else {
                throw std::invalid_argument("NeoGFF exports only semantic XML or JSON. CSV/TSV flattened export is not supported for GFF files.");
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExportPatcher(bool package) {
        if (!model().loaded()) return;
        try {
            requireGenericGffPatcherModel(model(), "The active document");
            const auto originalPath = wxui::chooseOpenFile(this, "Select clean/unmodified GFF3 baseline", gffWildcard());
            if (!originalPath) return;

            GffModel original;
            original.load(*originalPath);
            requireMatchingGffPatcherModels(original, model());

            std::string defaultPatchName = model().filename().empty()
                ? originalPath->filename().string()
                : model().filename().filename().string();
            if (defaultPatchName.empty()) defaultPatchName = "modified.gff";
            const auto patchName = wxui::promptText(this,
                                                    "Patch Target Filename",
                                                    "GFF filename to patch in the user's install:",
                                                    defaultPatchName);
            if (!patchName || patchName->empty()) return;

            auto project = neotsl::diffGffFlatTable(
                original.toTable(), model().toTable(), *patchName, package, *originalPath);
            neotsl::throwIfUnsupported(project);

            if (package) {
                const auto outputDir = wxui::chooseDirectory(this, "Choose tslpatchdata package folder");
                if (!outputDir) return;
                neotsl::writePackage(project, *outputDir, true);
                wxui::showMessage(this,
                                  "TSL/HoloPatcher Package",
                                  "Wrote changes.ini and staged the clean GFF baseline in:\n" + pathText(*outputDir));
            } else {
                const auto output = wxui::chooseSaveFile(
                    this,
                    "Save TSL/HoloPatcher GFF fragment",
                    "INI files (*.ini)|*.ini|All files (*.*)|*.*",
                    "gff_fragment.ini");
                if (!output) return;
                neotsl::writeFragment(project, *output);
                wxui::showMessage(this,
                                  "TSL/HoloPatcher Fragment",
                                  "Wrote a GFFList fragment to:\n" + pathText(*output));
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCopyCells(wxCommandEvent&) {
        if (!wxTheClipboard->Open()) return;
        if (isTreeView(viewMode())) {
            neotabular::Table copied;
            const int row = viewState().selectedLogicalRow;
            if (row >= 0 && row < static_cast<int>(displayRows_.size())) {
                const auto& item = displayRows_[static_cast<std::size_t>(row)];
                copied.rows.push_back({item.path, item.label, item.type, item.editable ? "yes" : "no", item.value, item.resolved});
            }
            wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(neotabular::serializeDelimited(copied, '\t'))));
            wxTheClipboard->Close();
            return;
        }
        if (grid_ == nullptr) {
            wxTheClipboard->Close();
            return;
        }
        int top = grid_->GetGridCursorRow();
        int left = grid_->GetGridCursorCol();
        int bottom = top;
        int right = left;
        const wxGridCellCoordsArray blockTop = grid_->GetSelectionBlockTopLeft();
        const wxGridCellCoordsArray blockBottom = grid_->GetSelectionBlockBottomRight();
        if (!blockTop.IsEmpty() && !blockBottom.IsEmpty()) {
            top = blockTop[0].GetRow();
            left = blockTop[0].GetCol();
            bottom = blockBottom[0].GetRow();
            right = blockBottom[0].GetCol();
        }
        neotabular::Table copied;
        if (top >= 0 && left >= 0 && bottom >= top && right >= left) {
            for (int r = top; r <= bottom; ++r) {
                std::vector<std::string> row;
                for (int c = left; c <= right; ++c) row.push_back(wxui::toStd(grid_->GetCellValue(r, c)));
                copied.rows.push_back(std::move(row));
            }
        }
        wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(neotabular::serializeDelimited(copied, '\t'))));
        wxTheClipboard->Close();
    }

    void onPasteCells(wxCommandEvent&) {
        if (!model().loaded() || !wxTheClipboard->Open()) return;
        if (!wxTheClipboard->IsSupported(wxDF_TEXT)) {
            wxTheClipboard->Close();
            return;
        }
        wxTextDataObject data;
        wxTheClipboard->GetData(data);
        wxTheClipboard->Close();
        try {
            const auto pasted = neotabular::parseDelimited(wxui::toStd(data.GetText()), '\t');
            if (isTreeView(viewMode())) {
                const int row = viewState().selectedLogicalRow;
                if (row >= 0 && row < static_cast<int>(displayRows_.size()) && displayRows_[static_cast<std::size_t>(row)].editable &&
                    !pasted.rows.empty() && !pasted.rows.front().empty()) {
                    model().setValue(displayRows_[static_cast<std::size_t>(row)].path, pasted.rows.front().back());
                }
                refreshAll();
                return;
            }
            if (grid_ == nullptr) {
                refreshAll();
                return;
            }
            const int startRow = grid_->GetGridCursorRow();
            const int startCol = grid_->GetGridCursorCol();
            for (std::size_t r = 0; r < pasted.rows.size(); ++r) {
                const int gridRow = startRow + static_cast<int>(r);
                if (gridRow < 0 || gridRow >= static_cast<int>(displayRows_.size())) continue;
                for (std::size_t c = 0; c < pasted.rows[r].size(); ++c) {
                    const int col = startCol + static_cast<int>(c);
                    const std::size_t logicalColumn = actualColumnForVisible(col);
                    if (logicalColumn == kColValue && displayRows_[static_cast<std::size_t>(gridRow)].editable) {
                        model().setValue(displayRows_[static_cast<std::size_t>(gridRow)].path, pasted.rows[r][c]);
                    }
                }
            }
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshAll();
        }
    }

    bool save(bool saveAs) {
        if (!model().loaded()) return false;
        try {
            std::filesystem::path target = model().filename();
            if (saveAs || target.empty()) {
                const std::string defaultName = target.empty()
                    ? std::string("new.") + preferredGffExtensionForType(model().fileType())
                    : target.filename().string();
                auto chosen = wxui::chooseSaveFile(
                    this, "Save GFF-backed resource", gffWildcard(), defaultName);
                if (!chosen) return false;
                target = *chosen;
            }
            model().save(target);
            rememberRecentFile(target);
            neogames::resolver().inferFromOpenedPath(target);
            refreshAll();
            return true;
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            return false;
        }
    }

    int selectedGridRow() const {
        if (isTreeView(viewMode())) return viewState().selectedLogicalRow;
        if (!grid_) return -1;
        if (!grid_->GetSelectedRows().IsEmpty()) return grid_->GetSelectedRows()[0];
        const int row = grid_->GetGridCursorRow();
        return row >= 0 && row < static_cast<int>(displayRows_.size()) ? row : -1;
    }

    std::string selectedContainerPath() const {
        const int row = selectedGridRow();
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) {
            return isTreeView(viewMode()) ? viewState().selectedPath : std::string{};
        }
        const auto& item = displayRows_[static_cast<std::size_t>(row)];
        if (item.type == "Struct" || item.type == "List") return item.path;
        return parentPathOf(item.path);
    }

    void onAddField(wxCommandEvent&) {
        if (!model().loaded()) return;
        AddFieldDialog dialog(this, selectedContainerPath());
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        try {
            model().addField(dialog.parentPath(), dialog.label(), dialog.type(), dialog.value(), dialog.structTypeId());
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onDeleteField(wxCommandEvent&) {
        const int row = selectedGridRow();
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) return;
        const auto path = displayRows_[static_cast<std::size_t>(row)].path;
        if (path.empty() || !displayRows_[static_cast<std::size_t>(row)].deletable) return;
        if (!wxui::confirm(this, "Delete Field", "Delete selected field?\n" + path)) return;
        try {
            model().deleteField(path);
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCellChanged(wxGridEvent& event) {
        const int row = event.GetRow();
        const int col = event.GetCol();
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) {
            event.Skip();
            return;
        }
        std::size_t logicalColumn = 0;
        try {
            logicalColumn = actualColumnForVisible(col);
        } catch (const std::exception&) {
            event.Skip();
            return;
        }
        if (logicalColumn != kColValue) {
            event.Skip();
            return;
        }
        const auto item = displayRows_[static_cast<std::size_t>(row)];
        if (!item.editable) {
            refreshAll();
            return;
        }
        try {
            model().setValue(item.path, wxui::toStd(grid_->GetCellValue(row, col)));
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshAll();
        }
    }

    void onTreeActivated(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        auto* data = (tree_ && item.IsOk()) ? dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item)) : nullptr;
        if (!data || data->rowIndex() < 0 || data->rowIndex() >= static_cast<int>(displayRows_.size())) {
            if (tree_ && item.IsOk() && tree_->ItemHasChildren(item)) {
                if (tree_->IsExpanded(item)) tree_->Collapse(item);
                else tree_->Expand(item);
            }
            return;
        }

        const auto& row = displayRows_[static_cast<std::size_t>(data->rowIndex())];
        if (!row.editable) {
            if (tree_ && item.IsOk() && tree_->ItemHasChildren(item)) {
                if (tree_->IsExpanded(item)) tree_->Collapse(item);
                else tree_->Expand(item);
            }
            return;
        }

        auto value = wxui::promptText(this, "Edit GFF Value", row.label + " (" + row.type + "):", row.value);
        if (!value) return;
        try {
            model().setValue(row.path, *value);
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshAll();
        }
    }

    void onCloseTab(wxCommandEvent&) { closeDocumentTab(activeDocumentIndex_); }

    void onCloseOtherTabs(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        for (std::size_t i = documents_.size(); i-- > 0;) {
            if (i != activeDocumentIndex_ && !closeDocumentTab(i)) return;
        }
    }

    void onNextTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(true);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onPreviousTab(wxCommandEvent&) {
        if (documentTabs_ == nullptr || documentTabs_->GetPageCount() < 2) return;
        tabSwitchInProgress_ = true;
        documentTabs_->AdvanceSelection(false);
        tabSwitchInProgress_ = false;
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::currentPage(documentTabs_));
        if (index != neotabs::npos) selectDocumentTab(index);
    }

    void onToggleDarkMode(wxCommandEvent&) {
        darkMode_ = darkModeItem_ && darkModeItem_->IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        applyDarkMode();
    }

    void onClose(wxCloseEvent& event) {
        if (event.CanVeto() && !confirmCloseAllTabs()) {
            event.Veto();
            return;
        }
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    void refreshTree() {
        if (!tree_) return;

        viewState().selectedLogicalRow = -1;
        viewState().selectedPath.clear();
        treeRowItems_.assign(displayRows_.size(), wxTreeItemId{});
        tree_->DeleteAllItems();

        const std::string rootText = model().loaded()
            ? (pathText(model().filename()).empty() ? std::string("New GFF File") : pathText(model().filename()))
            : std::string("No GFF loaded");
        const wxTreeItemId root = tree_->AddRoot(wxui::toWx(rootText), -1, -1, new GffTreeItemData(std::string{}, -1));

        if (!model().loaded()) {
            tree_->Expand(root);
            return;
        }

        std::map<std::string, wxTreeItemId> pathToItem;
        pathToItem[std::string{}] = root;

        std::function<wxTreeItemId(const std::string&)> ensureItem = [&](const std::string& path) -> wxTreeItemId {
            if (path.empty()) return root;
            const auto found = pathToItem.find(path);
            if (found != pathToItem.end()) return found->second;
            const wxTreeItemId parent = ensureItem(treeParentPathOf(path));
            const wxTreeItemId item = tree_->AppendItem(parent, wxui::toWx(pathLeaf(path)), -1, -1,
                                                        new GffTreeItemData(path, -1));
            pathToItem[path] = item;
            return item;
        };

        for (std::size_t i = 0; i < displayRows_.size(); ++i) {
            const auto& row = displayRows_[i];
            const std::string parentPath = treeParentPathOf(row.path);
            const wxTreeItemId parent = ensureItem(parentPath);

            wxTreeItemId item;
            const auto found = pathToItem.find(row.path);
            if (found != pathToItem.end() && found->second != root) {
                item = found->second;
                tree_->SetItemText(item, wxui::toWx(treeTextForRow(row)));
                if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
                    data->setRowIndex(static_cast<int>(i));
                } else {
                    tree_->SetItemData(item, new GffTreeItemData(row.path, static_cast<int>(i)));
                }
            } else {
                item = tree_->AppendItem(parent, wxui::toWx(treeTextForRow(row)), -1, -1,
                                         new GffTreeItemData(row.path, static_cast<int>(i)));
                pathToItem[row.path] = item;
            }
            treeRowItems_[i] = item;
        }

        tree_->Expand(root);
        if (isTreeView(viewMode()) && !displayRows_.empty() && treeRowItems_[0].IsOk()) {
            tree_->SelectItem(treeRowItems_[0]);
        }
    }

    void expandTreeRecursive(const wxTreeItemId& item) {
        if (!tree_ || !item.IsOk()) return;
        tree_->Expand(item);
        wxTreeItemIdValue cookie;
        wxTreeItemId child = tree_->GetFirstChild(item, cookie);
        while (child.IsOk()) {
            expandTreeRecursive(child);
            child = tree_->GetNextChild(item, cookie);
        }
    }

    void collapseTreeRecursive(const wxTreeItemId& item) {
        if (!tree_ || !item.IsOk()) return;
        wxTreeItemIdValue cookie;
        wxTreeItemId child = tree_->GetFirstChild(item, cookie);
        while (child.IsOk()) {
            collapseTreeRecursive(child);
            child = tree_->GetNextChild(item, cookie);
        }
        tree_->Collapse(item);
    }

    bool gffRowPassesCurrentFilters(const neotabular::Table& table, std::size_t row) const {
        if (row >= table.rows.size()) return false;
        if (!viewState().filterTerm.empty() && !neotabular::rowMatches(table, table.rows[row], viewState().filterTerm)) {
            return false;
        }
        return neoview::rowPassesColumnFilters(viewState(), [&](std::size_t logicalColumn) {
            return logicalColumn < table.rows[row].size() ? table.rows[row][logicalColumn] : std::string();
        });
    }

    std::string gffCellText(const GffFieldRow& row, std::size_t logicalColumn) const {
        switch (logicalColumn) {
        case kColPath: return row.path;
        case kColLabel: return row.label;
        case kColType: return row.type;
        case kColEditable: return row.editable ? "yes" : "no";
        case kColValue: return row.value;
        case kColResolved: return row.resolved;
        default: return {};
        }
    }

    void refreshAll() {
        displayRows_.clear();
        const auto modelRows = model().rows();
        neoview::removeColumnFiltersOutsideRange(viewState(), kColumnCount);
        std::vector<std::size_t> visibleLogicalRows;
        visibleLogicalRows.reserve(modelRows.size());
        if (model().loaded()) {
            const auto table = model().toTable();
            for (std::size_t i = 0; i < modelRows.size(); ++i) {
                if (gffRowPassesCurrentFilters(table, i)) {
                    displayRows_.push_back(modelRows[i]);
                    visibleLogicalRows.push_back(i);
                }
            }
        }
        neoview::setRowsFromLogicalRows(viewState(), std::move(visibleLogicalRows));
        neoview::ensureIdentityColumns(viewState(), kColumnCount);
        filePath_->SetValue(wxui::toWx(pathText(model().filename())));
        typeText_->SetValue(wxui::toWx(typeVersionText(model())));
        if (tlkPath_) tlkPath_->SetValue(wxui::toWx(model().tlk().loaded() ? pathText(model().tlk().filename()) : std::string{}));
        SetTitle(wxui::toWx("NeoGFF v1.0.0" + std::string(model().dirty() ? " *" : "") + " (GFF editor)"));
        updateActiveTabTitle();

        if (grid_->GetNumberRows() > 0) grid_->DeleteRows(0, grid_->GetNumberRows());
        if (!displayRows_.empty()) grid_->AppendRows(static_cast<int>(displayRows_.size()));
        const int wantedCols = static_cast<int>(viewState().visualToLogicalColumns.size());
        for (int visualCol = 0; visualCol < wantedCols; ++visualCol) {
            const std::size_t logicalColumn = actualColumnForVisible(visualCol);
            std::string label = gffColumnLabel(logicalColumn);
            if (neoview::findColumnFilter(viewState(), logicalColumn) != nullptr) label += " *";
            grid_->SetColLabelValue(visualCol, wxui::toWx(label));
        }
        for (std::size_t i = 0; i < displayRows_.size(); ++i) {
            const int r = static_cast<int>(i);
            const auto& row = displayRows_[i];
            for (int visualCol = 0; visualCol < wantedCols; ++visualCol) {
                const std::size_t logicalColumn = actualColumnForVisible(visualCol);
                grid_->SetCellValue(r, visualCol, wxui::toWx(gffCellText(row, logicalColumn)));
                grid_->SetReadOnly(r, visualCol, !(logicalColumn == kColValue && row.editable));
            }
        }
        const int editableVisualCol = neoview::visualColumnForLogical(viewState(), kColEditable);
        if (editableVisualCol >= 0) grid_->AutoSizeColumn(editableVisualCol, false);
        refreshTree();
        applyDarkMode();
        if (GetStatusBar()) {
            wxui::setStatusText(*this, wxui::toWx(model().loaded() ? pathText(model().filename()) : std::string("No GFF loaded")), 0);
            const auto totalRows = model().loaded() ? model().rows().size() : 0u;
            std::string detail = std::to_string(displayRows_.size()) + "/" + std::to_string(totalRows) + " rows";
            const std::string columnFilters = neoview::columnFilterSummary(viewState());
            if (!columnFilters.empty()) detail += "; filters: " + columnFilters;
            if (model().tlk().loaded()) detail += "; TLK " + std::to_string(model().tlk().count()) + " entries";
            else if (!tlkAutoLoadWarning().empty()) detail += "; " + tlkAutoLoadWarning();
            wxui::setStatusText(*this, wxui::toWx(detail), 1);
        }
    }

    void applyDarkMode() {
        if (darkModeItem_) darkModeItem_->Check(darkMode_);
        if (flatGridViewItem_) flatGridViewItem_->Check(viewMode() == GffViewMode::FlatGrid);
        if (elementTreeViewItem_) elementTreeViewItem_->Check(viewMode() == GffViewMode::ElementTree);
        wxui::applyTheme(this, darkMode_);
        if (grid_) wxui::applyGridTheme(*grid_, darkMode_);
        if (tree_) wxui::applyTreeTheme(*tree_, darkMode_);
        applyFontScale();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
    }

    void changeFontScaleSteps(int steps) {
        const double next = neoview::steppedFontScale(fontScale_, steps);
        if (neoview::fontScalePercent(next) == neoview::fontScalePercent(fontScale_)) return;
        fontScale_ = next;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onIncreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(1);
    }
    void onDecreaseFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        changeFontScaleSteps(-1);
    }
    void onResetFontScale(wxCommandEvent&) {
        fontScaleWheelFilter_.reset();
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }



    neosettings::AppSettings settings_{kAppName};
    wxMenu* recentFilesMenu_ = nullptr;
    wxAuiNotebook* documentTabs_ = nullptr;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    std::vector<GffFieldRow> displayRows_;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* typeText_ = nullptr;
    wxTextCtrl* tlkPath_ = nullptr;
    wxTextCtrl* filterText_ = nullptr;
    wxPanel* viewPanel_ = nullptr;
    wxBoxSizer* viewSizer_ = nullptr;
    wxGrid* grid_ = nullptr;
    int contextVisualColumn_ = -1;
    wxTreeCtrl* tree_ = nullptr;
    std::vector<wxTreeItemId> treeRowItems_;
    wxMenuItem* flatGridViewItem_ = nullptr;
    wxMenuItem* elementTreeViewItem_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;
    GffViewMode pendingViewMode_ = GffViewMode::FlatGrid;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};

class NeoGFFApp final : public wxApp {
public:
    bool OnInit() override {
        const bool smokeTest =
            argc > 1 && wxString(argv[1]) == wxString::FromUTF8("--smoke-test");

        auto* frame = new NeoGFFFrame();
        frame->Show(!smokeTest);
        if (!smokeTest && argc > 1) {
            frame->openStartupFile(neosettings::pathFromWx(wxString(argv[1])));
        }
        if (smokeTest) {
            CallAfter([frame]() {
                frame->Destroy();
                if (wxTheApp != nullptr) wxTheApp->ExitMainLoop();
            });
        }
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoGFFApp);
