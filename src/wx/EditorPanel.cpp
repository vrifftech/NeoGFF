#include "GFFEditorPanel.hpp"
#include <neoshared/GffResourceDocument.hpp>
#include "core/AppModel.hpp"
#include "wx_ui.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoDocumentTabs.hpp"
#include "NeoSettings.hpp"
#include "NeoPatcherExport.hpp"
#include "NeoTreeState.hpp"
#include "NeoViewState.hpp"

#include "TabularData.hpp"
#include "TslPatcher.hpp"
#include "core/GffJson.hpp"
#include "core/Version.hpp"

#include <wx/aui/auibook.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/sizer.h>
#include <wx/treectrl.h>
#include <wx/wx.h>
#include <wx/wupdlock.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static_assert(wxui::kPatcherExportUiApiVersion >= 3u,
              "NeoGFF requires the exact-INI/Fragment patch-export UI from the current neoshared checkout.");
#if defined(__EMSCRIPTEN__)
static_assert(neobrowser::kBrowserFileApiVersion >= 10u,
              "NeoGFF requires owned browser imports and transactional write-back from the current neoshared checkout.");
#endif

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

bool supportedGffPatcherContent(const std::string& type) {
    static const std::unordered_set<std::string> supported = {"ARE", "BIC", "BTC", "BTD", "BTE", "BTI", "BTM", "BTP", "BTT",
        "FAC", "GFF", "GIT", "GUI", "IFO", "INV", "ITP", "JRL", "NFO",
        "PT", "PTH", "GVT", "UTC", "UTD", "UTE", "UTI", "UTM", "UTP",
        "UTS", "UTT", "UTW"};
    return supported.count(normalizedGffPatcherType(type)) != 0u;
}

bool containsJadeStringRef(const GffModel& model) {
    const auto table = model.toTable();
    auto typeColumn = std::find(table.columns.begin(), table.columns.end(), "Type");
    if (typeColumn == table.columns.end()) return false;
    const std::size_t index = static_cast<std::size_t>(std::distance(table.columns.begin(), typeColumn));
    for (const auto& row : table.rows) {
        if (index < row.size() && normalizedGffPatcherType(row[index]) == "JADESTRINGREF") return true;
    }
    return false;
}

void requireGenericGffPatcherModel(const GffModel& model, const std::string& role) {
    if (!model.loaded()) throw std::runtime_error(role + " is not loaded.");
    if (model.gff().isGff4()) {
        throw std::runtime_error(
            role + " is GFF4. TSLPatcher/HoloPatcher [GFFList] output supports classic GFF V3.2 files only.");
    }
    if (normalizedGffPatcherType(model.version()) != "V3.2") {
        throw std::runtime_error(
            role + " is " + model.version() + ". Original TSLPatcher and HoloPatcher 1.7 share support only for classic GFF V3.2 resources.");
    }
    const std::string type = normalizedGffPatcherType(model.fileType());
    if (type == "DLG") {
        throw std::runtime_error(
            role + " is a DLG file. Open it in NeoDLG and use the DLG-aware patcher exporter so dialogue indexes remain dynamic.");
    }
    if (!supportedGffPatcherContent(type)) {
        throw std::runtime_error(
            role + " uses GFF content type " + type + ", which is not recognized by both original TSLPatcher and HoloPatcher 1.7.");
    }
    if (containsJadeStringRef(model)) {
        throw std::runtime_error(
            role + " contains JadeStringRef fields, which neither patcher can encode in GFFList instructions.");
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

enum : int {
    ID_New = wxID_HIGHEST + 12000,
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
    ID_ClearAllFilters,
    ID_ImportXml,
    ID_ImportJson,
    ID_ExportXml,
    ID_ExportJson,
    ID_ExportPatcher,
    ID_ExpandTree,
    ID_CollapseTree,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_ElementTree
};

constexpr int ID_ModuleExit = wxID_HIGHEST + 12450;
constexpr int ID_ModuleAbout = wxID_HIGHEST + 12451;
constexpr int kRecentFileBaseId = wxID_HIGHEST + 12500;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

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
        wxui::configureResponsiveWindow(*this, wxSize(620, 320), wxSize(420, 240));
        CentreOnParent();
        wxui::constrainWindowToDisplay(*this);
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

class NeoGFFPanelImpl final : public neogff::ui::GFFEditorPanel {
public:
    NeoGFFPanelImpl(wxWindow* parent, neomodules::Context context)
        : GFFEditorPanel(parent, std::move(context)) {
        buildMenus();
        buildWindow();
        createModuleStatusBar(2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        if (!context_.embedded) fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        applyDarkMode();
        createDocumentTab(true);
        tryLoadCachedTlk();
        refreshAll();
    }



    bool activateResource(const std::string& identity) override {
        if (identity.empty()) return false;
        for (std::size_t i=0; i<documents_.size(); ++i)
            if (documents_[i].resourceIdentity == identity) { selectDocumentTab(i); return true; }
        return false;
    }
    std::size_t documentCount() const override { return documents_.size(); }
    bool openFile(const std::filesystem::path& path) override { return openModelPath(path, false); }
    neogff::GffModel* activeModel() override { return hasActiveDocument() ? &model() : nullptr; }
    void refreshActiveDocument() override { if (hasActiveDocument()) refreshAll(); }
    void setAppearance(bool dark, double scale) override { darkMode_=dark; fontScale_=scale; applyDarkMode(); }
    bool canClose() override {
        if (browserSaveActive_) return false;
        return confirmCloseAllTabs();
    }
    std::vector<std::filesystem::path> openPaths() const override {
        std::vector<std::filesystem::path> result;
        for (const auto& document : documents_) {
            const auto path = documentFilename(document);
            if (!path.empty()) result.push_back(path);
        }
        return result;
    }
    bool openResource(neoshared::ResourceDocument input) override {
        if (activateResource(input.identity)) return true;
        auto candidate = std::make_unique<GffModel>();
        neoshared::loadGffResource(input, candidate->gff(), {});
        // Parse before creating/replacing a tab, so a failed load leaves the UI intact.
        ensureDocumentTabForOpen();
        auto& document = activeDocument();
        document.model = std::move(candidate);
        document.logicalFilename.clear();
        document.resourceIdentity = std::move(input.identity);
        document.sourceDescription = std::move(input.sourceDescription);
        document.protectedInputs = std::move(input.protectedInputs);
        document.untitledName = std::move(input.fileName);
#if defined(__EMSCRIPTEN__)
        document.sourceImport.reset();
#endif
        viewState().resetForNewDocument();
        viewState().preferredViewMode = "ElementTree";
        document.treeState.reset();
        treeRenderedDocumentPage_ = nullptr;
        if (filterText_) filterText_->ChangeValue(wxString{});
        tryLoadCachedTlk();

        for (const auto& source : document.protectedInputs) {
            tryLoadResolvedTlkForPath(source);
            if (model().tlk().loaded()) break;
        }
        refreshAll();
        setModuleStatusText("Archive snapshot: " + document.sourceDescription + ". Save As creates a separate working file.");
        return true;
    }
    bool saveActiveAs(const std::filesystem::path& path) override {
        if (!hasActiveDocument() || !model().gff().loaded() || path.empty()) return false;
        checkDestination(path);
        return save(false, {}, path);
    }

private:
    void checkOutput(const std::filesystem::path& path, bool exporting = false) const {
        validateHostOutput(path);
        for (const auto& document : documents_) {
            neoshared::checkResourceOutput(path, document.protectedInputs);
            if ((exporting || &document != &activeDocument()) &&
                neoshared::sameResourcePath(path, documentFilename(document)))
                throw std::runtime_error("That destination belongs to an open document. Choose a separate working file.");
        }
    }
    void checkDestination(const std::filesystem::path& path) const {
        checkOutput(path);
        neoshared::checkGffOutputType(path, {});
    }


    using SaveCompletion = std::function<void(bool)>;
    using DeferredAction = std::function<void()>;

    struct DocumentTab {
        std::unique_ptr<GffModel> model = std::make_unique<GffModel>();
        std::filesystem::path logicalFilename;
        std::string resourceIdentity;
        std::string sourceDescription;
        std::vector<std::filesystem::path> protectedInputs;
        neoview::DocumentViewState viewState;
        neotree::TreeViewState treeState;
        std::string tlkAutoLoadWarning;
        std::string untitledName = "Untitled GFF";
        wxWindow* tabPage = nullptr;
        bool saveInProgress = false;
#if defined(__EMSCRIPTEN__)
        neobrowser::BrowserImportLease sourceImport;
#endif
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
    std::string& tlkAutoLoadWarning() { return activeDocument().tlkAutoLoadWarning; }
    const std::string& tlkAutoLoadWarning() const { return activeDocument().tlkAutoLoadWarning; }

    std::filesystem::path documentFilename(const DocumentTab& tab) const {
        if (!tab.logicalFilename.empty()) return tab.logicalFilename;
        return tab.model ? tab.model->filename() : std::filesystem::path{};
    }

    bool tabDirty(const DocumentTab& tab) const {
        return tab.saveInProgress || (tab.model && tab.model->dirty());
    }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(documentFilename(tab), tab.untitledName);
    }

    void updateDocumentTabTitle(DocumentTab& document) {
        neotabs::setTabLabel(documentTabs_, document.tabPage,
                             tabDisplayName(document), tabDirty(document));
    }

    void updateActiveTabTitle() {
        if (!hasActiveDocument()) return;
        updateDocumentTabTitle(activeDocument());
    }

    std::string treeItemKey(const wxTreeItemId& item) const {
        if (!tree_ || !item.IsOk()) return {};
        if (item == tree_->GetRootItem()) return "$root";
        if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
            return data->path();
        }
        return {};
    }

    void captureRenderedTreeState() {
        if (!tree_ || !hasActiveDocument() ||
            treeRenderedDocumentPage_ != activeDocument().tabPage) {
            return;
        }
        neotree::captureTreeViewState(
            *tree_, activeDocument().treeState,
            [this](const wxTreeItemId& item) { return treeItemKey(item); });
    }

    void selectDocumentTab(std::size_t index) {
        if (documentTabs_ == nullptr || index >= documents_.size()) return;
        if (hasActiveDocument() && index != activeDocumentIndex_) captureRenderedTreeState();
        tabSwitchInProgress_ = true;
        const bool selected = neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        if (!selected) return;
        activeDocumentIndex_ = index;
        if (filterText_ != nullptr) {
            filterText_->ChangeValue(wxui::toWx(viewState().filterTerm));
        }
        refreshAll();
    }

    void createDocumentTab(bool select = true) {
        if (select && hasActiveDocument()) captureRenderedTreeState();
        DocumentTab tab;
        tab.model = std::make_unique<GffModel>();
        tab.viewState.resetForNewDocument();
        tab.viewState.preferredViewMode = "ElementTree";
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

#if defined(__EMSCRIPTEN__)
    using BrowserImportCallback = std::function<void(neobrowser::BrowserImportLease)>;

    void requestBrowserImport(const std::string& title,
                              const std::string& accept,
                              bool multiple,
                              BrowserImportCallback callback) {
        wxWeakRef<NeoGFFPanelImpl> weakSelf(this);
        neobrowser::requestOpenFilesOwned(
            title, accept, multiple,
            [weakSelf, callback = std::move(callback)](
                neobrowser::OwnedOpenFilesResult result) mutable {
                if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                auto* const frame = weakSelf.get();
                if (!result.error.empty()) {
                    wxMessageBox(wxui::toWx(result.error), "File Open Error",
                                 wxOK | wxICON_ERROR, frame);
                    return;
                }
                if (result.cancelled()) return;
                callback(std::move(result.import));
            });
    }

    static bool importOwnsPath(const neobrowser::BrowserImportLease& import,
                               const std::filesystem::path& path) {
        return std::find(import.paths().begin(), import.paths().end(), path) !=
               import.paths().end();
    }
#endif

    bool confirmCloseDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (documents_[index].saveInProgress) {
            wxui::showMessage(this, "Save in progress",
                              "Finish the browser save transaction before closing this tab.");
            return false;
        }
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocumentTab(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocumentTab(index)) return false;

        wxWindow* const page = documents_[index].tabPage;
        if (treeRenderedDocumentPage_ == page) treeRenderedDocumentPage_ = nullptr;
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
        if (!context_.embedded) file->Append(ID_ModuleExit, "E&xit");

        auto* import = new wxMenu;
        import->Append(ID_ImportXml, "Import &XML...");
        import->Append(ID_ImportJson, "Import &JSON...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportXml, "Export as &XML...");
        exportMenu->Append(ID_ExportJson, "Export as &JSON...");
        exportMenu->AppendSeparator();
        exportMenu->Append(ID_ExportPatcher, "Export TSL/HoloPatcher Instructions...");

        auto* edit = new wxMenu;
        edit->Append(ID_CopyCells, "&Copy Selected Value	Ctrl-C");
        edit->Append(ID_PasteCells, "&Paste Selected Value	Ctrl-V");
        edit->AppendSeparator();
        edit->Append(ID_Filter, "&Filter/Search...	Ctrl-F");
        edit->Append(ID_ClearAllFilters, "Clear &Filter");
        edit->AppendSeparator();
        edit->Append(ID_AddField, "&Add Field...");
        edit->Append(ID_DeleteField, "&Delete Selected Field");

        auto* view = new wxMenu;
        view->Append(ID_ExpandTree, "E&xpand GFF Tree");
        view->Append(ID_CollapseTree, "&Collapse GFF Tree");
        view->AppendSeparator();
        if (!context_.embedded) {
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "&Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Font Size	Ctrl++");
        view->Append(ID_FontDecrease, "Decrease Font Size	Ctrl+-");
        view->Append(ID_FontReset, "Reset Font Size	Ctrl+0");
        }

        auto* help = new wxMenu;
        help->Append(ID_ModuleAbout, "&About");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(import, "&Import");
        bar->Append(exportMenu, "&Export");
        bar->Append(edit, "&Edit");
        bar->Append(view, "&View");
        if (!context_.embedded) bar->Append(help, "&Help"); else delete help;
        setModuleMenus(bar);

        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onNew, this, ID_New);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onOpenRecent, this, kRecentFileBaseId, kRecentFileBaseId + neosettings::kMaxRecentFiles - 1);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onClearRecentFiles, this, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onAddField, this, ID_AddField);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onDeleteField, this, ID_DeleteField);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onCopyCells, this, ID_CopyCells);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onPasteCells, this, ID_PasteCells);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onFilterPrompt, this, ID_Filter);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onClearFilter, this, ID_ClearFilter);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onClearAllFilters, this, ID_ClearAllFilters);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Xml); }, ID_ImportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(neotabular::Format::Json); }, ID_ImportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Xml); }, ID_ExportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(neotabular::Format::Json); }, ID_ExportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExportPatcher(); }, ID_ExportPatcher);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onExpandTree, this, ID_ExpandTree);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onCollapseTree, this, ID_CollapseTree);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoGFFPanelImpl::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, ID_ModuleExit);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoGFF", std::string("NeoGFF v") + kVersion + "\nNative wxWidgets BioWare GFF tree editor\n\nA special thanks to everyone in the KOTOR modding community that has contributed their work, knowledge, and creativity to making tools, mods, and guides over the last 20+ years");
        }, ID_ModuleAbout);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onNew, this, ID_New);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onAddField, this, ID_AddField);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onDeleteField, this, ID_DeleteField);
        Bind(wxEVT_BUTTON, &NeoGFFPanelImpl::onClearFilter, this, ID_ClearFilter);
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

        tree_ = new wxTreeCtrl(viewPanel_, ID_ElementTree, wxDefaultPosition, wxDefaultSize,
                               wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        tree_->SetName("NeoGFF resource tree");

        viewSizer_->Add(tree_, 1, wxEXPAND);
        viewPanel_->SetSizer(viewSizer_);
        root->Add(viewPanel_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);
        auto* moduleLayout = new wxBoxSizer(wxVERTICAL);
        moduleLayout->Add(panel, 1, wxEXPAND);
        SetSizer(moduleLayout);
        if (filterText_) filterText_->Bind(wxEVT_TEXT, &NeoGFFPanelImpl::onFilterText, this);
        Bind(wxEVT_TREE_SEL_CHANGED, &NeoGFFPanelImpl::onTreeSelectionChanged, this, ID_ElementTree);
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoGFFPanelImpl::onTreeActivated, this, ID_ElementTree);
        Bind(wxEVT_TREE_ITEM_EXPANDING, &NeoGFFPanelImpl::onTreeItemExpanding, this, ID_ElementTree);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoGFFPanelImpl::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoGFFPanelImpl::onDocumentTabCloseRequested, this);
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
        for (std::size_t i=0; i<documents_.size(); ++i) {
            if (neoshared::sameResourcePath(path, documentFilename(documents_[i]))) {
                selectDocumentTab(i); return true;
            }
        }

        (void)checkDirty;
        auto candidate = std::make_unique<GffModel>();
        candidate->load(path);
        ensureDocumentTabForOpen();
        activeDocument().model = std::move(candidate);
        activeDocument().logicalFilename = path;
        activeDocument().resourceIdentity.clear();
        activeDocument().sourceDescription.clear();
        activeDocument().protectedInputs.clear();
#if defined(__EMSCRIPTEN__)
        activeDocument().sourceImport.reset();
#endif
        viewState().resetForNewDocument();
        viewState().preferredViewMode = "ElementTree";
        viewState().selectedLogicalRow = -1;
        viewState().filterTerm.clear();
        activeDocument().treeState.reset();
        if (treeRenderedDocumentPage_ == activeDocument().tabPage) treeRenderedDocumentPage_ = nullptr;
        if (filterText_ != nullptr && !filterText_->GetValue().empty()) {
            filterText_->ChangeValue(wxString{});
        }
        tryLoadResolvedTlkForPath(path);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        refreshAll();
        return true;
    }

#if defined(__EMSCRIPTEN__)
    bool openModelPath(const std::filesystem::path& path,
                       neobrowser::BrowserImportLease import,
                       bool checkDirty = true) {
        if (!openModelPath(path, checkDirty)) return false;
        activeDocument().sourceImport = std::move(import);
        return true;
    }
#endif

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

    bool maybeSave(DeferredAction afterSave = {}) {
#if !defined(__EMSCRIPTEN__)
        (void)afterSave;
#endif
        if (activeDocument().saveInProgress || browserSaveActive_) return false;
        if (!model().loaded() || !model().dirty()) return true;
        const int result = wxMessageBox("The current GFF has unsaved changes. Save it?", "Unsaved Changes",
                                        wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
        if (result == wxCANCEL) return false;
        if (result == wxYES) {
#if defined(__EMSCRIPTEN__)
            SaveCompletion completion;
            if (afterSave) {
                completion = [action = std::move(afterSave)](bool succeeded) mutable {
                    if (succeeded && action) action();
                };
            }
            (void)save(false, std::move(completion));
            return false;
#else
            return save(false);
#endif
        }
        return true;
    }

    void onNew(wxCommandEvent&) {
        auto type = wxui::promptText(this, "New GFF", "GFF file type:", "UTC");
        if (!type) return;
        try {
            createDocumentTab(true);
            model().newFile(*type);
            activeDocument().logicalFilename.clear();
#if defined(__EMSCRIPTEN__)
            activeDocument().sourceImport.reset();
#endif
            viewState().resetForNewDocument();
            viewState().preferredViewMode = "ElementTree";
            viewState().selectedLogicalRow = -1;
            viewState().filterTerm.clear();
            activeDocument().treeState.reset();
            if (treeRenderedDocumentPage_ == activeDocument().tabPage) treeRenderedDocumentPage_ = nullptr;
            if (filterText_ != nullptr && !filterText_->GetValue().empty()) {
                filterText_->ChangeValue(wxString{});
            }
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpenGff(const std::filesystem::path& initialDirectory = {}) {
#if defined(__EMSCRIPTEN__)
        (void)initialDirectory;
        requestBrowserImport(
            "Open GFF-backed resource",
            wxui::detail::wildcardToBrowserAccept(gffWildcard()),
            false,
            [this](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                try {
                    const std::filesystem::path selected = import.paths().front();
                    openModelPath(selected, std::move(import), false);
                } catch (const std::exception& ex) {
                    wxui::showError(this, ex);
                }
            });
#else
        auto file = wxui::chooseOpenFile(this, "Open GFF-backed resource", gffWildcard(), initialDirectory);
        if (!file) return;
        try {
            openModelPath(*file, false);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
#endif
    }

    void onOpen(wxCommandEvent&) {
        chooseAndOpenGff();
    }

    void loadTlkFromPath(const std::filesystem::path& file, bool rememberPath = true) {
        try {
            model().loadTlk(file);
            if (rememberPath) writeCachedTlkPath(file);
            else clearCachedTlkPath();
            tlkAutoLoadWarning().clear();
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onOpenTlk(wxCommandEvent&) {
#if defined(__EMSCRIPTEN__)
        if (!hasActiveDocument()) return;
        wxWindow* const targetPage = activeDocument().tabPage;
        requestBrowserImport(
            "Open optional TLK for resolved StrRef text",
            ".tlk",
            false,
            [this, targetPage](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                    wxui::showMessage(
                        this,
                        "TLK Load Cancelled",
                        "The active document changed while the TLK picker was open. Select the TLK again from the intended tab.");
                    return;
                }
                // TlkLookup owns its decoded contents after load. The temporary
                // browser import is released when this callback returns.
                loadTlkFromPath(import.paths().front(), false);
            });
#else
        auto file = wxui::chooseOpenFile(this, "Open optional TLK for resolved StrRef text", kTlkWildcard);
        if (!file) return;
        loadTlkFromPath(*file);
#endif
    }

    void onClearTlk(wxCommandEvent&) {
        model().clearTlk();
        clearCachedTlkPath();
        tlkAutoLoadWarning().clear();
        refreshAll();
    }

    void tryLoadCachedTlk() {
#if defined(__EMSCRIPTEN__)
        // Browser import paths are process-local and cannot be restored on a
        // later page load.
        clearCachedTlkPath();
        return;
#else
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
#endif
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

    void onClearFilter(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
    }

    void onClearAllFilters(wxCommandEvent&) {
        clearAllFiltersAndRefresh();
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

    void updateSelectionStateFromTreeItem(const wxTreeItemId& item) {
        viewState().selectedLogicalRow = -1;
        viewState().selectedPath.clear();
        if (tree_ && item.IsOk()) {
            if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
                viewState().selectedLogicalRow = data->rowIndex();
                viewState().selectedPath = data->path();
            }
        }
    }

    void onTreeSelectionChanged(wxTreeEvent& event) {
        if (!treeRefreshInProgress_) updateSelectionStateFromTreeItem(event.GetItem());
        event.Skip();
    }

    void importFromPath(neotabular::Format format, const std::filesystem::path& chosen) {
        try {
            if (format == neotabular::Format::Xml) {
                model().importXml(readTextFile(chosen));
            } else if (format == neotabular::Format::Json) {
                model().importXml(gffJsonToXml(readTextFile(chosen)));
            } else {
                throw std::invalid_argument("NeoGFF imports only semantic XML or JSON. CSV/TSV flattened import is not supported for GFF files.");
            }
            viewState().resetForNewDocument();
            viewState().preferredViewMode = "ElementTree";
            activeDocument().treeState.reset();
            if (treeRenderedDocumentPage_ == activeDocument().tabPage) treeRenderedDocumentPage_ = nullptr;
            if (filterText_) filterText_->ChangeValue("");
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onImport(neotabular::Format format) {
#if defined(__EMSCRIPTEN__)
        if (!hasActiveDocument()) return;
        wxWindow* const targetPage = activeDocument().tabPage;
        requestBrowserImport(
            "Import " + neotabular::formatName(format),
            format == neotabular::Format::Xml ? ".xml" : ".json",
            false,
            [this, targetPage, format](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                    wxui::showMessage(
                        this,
                        "Import Cancelled",
                        "The active document changed while the import picker was open. Start the import again from the intended tab.");
                    return;
                }
                importFromPath(format, import.paths().front());
            });
#else
        try {
            const auto chosen = wxui::chooseOpenFile(
                this,
                "Import " + neotabular::formatName(format),
                tableWildcardForFormat(format));
            if (!chosen) return;
            importFromPath(format, *chosen);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
#endif
    }

    void onExport(neotabular::Format format) {
        if (!model().loaded()) return;
        try {
            const auto chosen = wxui::chooseSaveFile(this, "Export " + neotabular::formatName(format), tableWildcardForFormat(format),
                                                   exportDefaultFilename(documentFilename(activeDocument()), format, "gff"));
            if (!chosen) return;
            if (format == neotabular::Format::Xml || format == neotabular::Format::Json) {
                if (neoview::hasAnyFilter(viewState())) {
                    throw std::invalid_argument("Semantic GFF XML/JSON export preserves hierarchy and does not support row filtering.");
                }
                const std::string xml = model().toXml();
                checkOutput(*chosen, true);
                writeTextFile(*chosen, format == neotabular::Format::Json ? gffXmlToJson(xml) : xml);
            } else {
                throw std::invalid_argument("NeoGFF exports only semantic XML or JSON. CSV/TSV flattened export is not supported for GFF files.");
            }
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void exportPatcherFromOriginal(const std::filesystem::path& originalPath) {
        try {
            requireGenericGffPatcherModel(model(), "The active document");
            GffModel original;
            original.load(originalPath);
            requireMatchingGffPatcherModels(original, model());

            const std::filesystem::path currentFilename = documentFilename(activeDocument());
            std::string defaultPatchName = currentFilename.empty()
                ? originalPath.filename().string()
                : currentFilename.filename().string();
            if (defaultPatchName.empty()) defaultPatchName = "modified.gff";
            const auto patchName = wxui::promptText(
                this,
                "Patch Target Filename",
                "GFF filename to patch in the user's install:",
                defaultPatchName);
            if (!patchName || patchName->empty()) return;

            const auto output = wxui::choosePatcherOutput(this);
            if (!output) return;
            const bool writeToIni = output->writesToIni();

            auto project = neotsl::diffGffFlatTable(
                original.toTable(), model().toTable(), *patchName, writeToIni, originalPath);
            neotsl::throwIfUnsupported(project);

            if (!writeToIni) {
                wxui::showIniFragmentDialog(
                    this,
                    "GFF Patcher INI Fragment",
                    project,
                    {*patchName});
                return;
            }

            const auto report = neotsl::writePackageToIni(project, output->iniPath, true);
            wxui::showMessage(
                this,
                "TSL/HoloPatcher Package",
                std::string(report.mergedExisting ? "Merged the generated GFF instructions into:\n"
                                                  : "Created the installer INI:\n") +
                    pathText(report.iniPath) +
                    "\n\nThe clean GFF baseline was staged beside the selected INI.");
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExportPatcher() {
        if (!model().loaded()) return;
        try {
            requireGenericGffPatcherModel(model(), "The active document");
#if defined(__EMSCRIPTEN__)
            wxWindow* const targetPage = activeDocument().tabPage;
            requestBrowserImport(
                "Select clean/unmodified GFF3 baseline",
                wxui::detail::wildcardToBrowserAccept(gffWildcard()),
                false,
                [this, targetPage](neobrowser::BrowserImportLease import) {
                    if (import.empty() || IsBeingDeleted()) return;
                    if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                        wxui::showMessage(
                            this,
                            "Patcher Export Cancelled",
                            "The active document changed while the baseline picker was open. Start the export again from the intended tab.");
                        return;
                    }
                    exportPatcherFromOriginal(import.paths().front());
                });
#else
            const auto originalPath = wxui::chooseOpenFile(
                this,
                "Select clean/unmodified GFF3 baseline",
                gffWildcard());
            if (!originalPath) return;
            exportPatcherFromOriginal(*originalPath);
#endif
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onCopyCells(wxCommandEvent&) {
        const int row = viewState().selectedLogicalRow;
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) return;
        if (!wxTheClipboard->Open()) return;
        wxTheClipboard->SetData(new wxTextDataObject(wxui::toWx(displayRows_[static_cast<std::size_t>(row)].value)));
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

        const int row = viewState().selectedLogicalRow;
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) return;
        const auto& selected = displayRows_[static_cast<std::size_t>(row)];
        if (!selected.editable) return;
        try {
            model().setValue(selected.path, wxui::toStd(data.GetText()));
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            refreshAll();
        }
    }

    bool save(bool saveAs, SaveCompletion completion = {}, const std::filesystem::path& forcedTarget = {}) {
        if (!model().loaded() || !hasActiveDocument()) return false;
        if (activeDocument().saveInProgress || browserSaveActive_) return false;
        try {
            DocumentTab& document = activeDocument();
            std::filesystem::path target = forcedTarget.empty() ? documentFilename(document) : forcedTarget;
            if ((saveAs && forcedTarget.empty()) || target.empty()) {
                const std::string defaultName = target.empty()
                    ? (!document.resourceIdentity.empty() ? document.untitledName : std::string("new.") + preferredGffExtensionForType(model().fileType()))
                    : target.filename().string();
                auto chosen = wxui::chooseSaveFile(
                    this, "Save GFF-backed resource", gffWildcard(), defaultName);
                if (!chosen) return false;
                target = *chosen;
            }

            checkDestination(target);
#if defined(__EMSCRIPTEN__)
            const bool wasDirty = document.model->dirty();
#endif
            document.model->save(target);

#if defined(__EMSCRIPTEN__)
            document.saveInProgress = true;
            browserSaveActive_ = true;
            updateDocumentTabTitle(document);
            refreshAll();
            Enable(false);

            wxWeakRef<NeoGFFPanelImpl> weakSelf(this);
            wxWindow* const targetPage = document.tabPage;
            neobrowser::requestDownloadFile(
                target,
                target.filename().string(),
                [weakSelf, targetPage, target, wasDirty,
                 completion = std::move(completion)](
                    neobrowser::DownloadResult result) mutable {
                    if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                    auto* const frame = weakSelf.get();
                    frame->browserSaveActive_ = false;
                    frame->Enable(true);

                    const std::size_t index = neotabs::findDocumentIndexForPage(
                        frame->documents_, targetPage);
                    if (index == neotabs::npos) return;

                    DocumentTab& savedDocument = frame->documents_[index];
                    savedDocument.saveInProgress = false;
                    if (!result.error.empty() || result.cancelled()) {
                        savedDocument.model->gff().dirty(wasDirty);
                        frame->updateDocumentTabTitle(savedDocument);
                        if (index == frame->activeDocumentIndex_) frame->refreshAll();
                        const std::string message = result.error.empty()
                            ? "The browser save transaction was cancelled."
                            : result.error;
                        wxMessageBox(wxui::toWx(message), "Save Failed",
                                     wxOK | wxICON_ERROR, frame);
                        if (completion) completion(false);
                        return;
                    }

                    if (result.ready()) {
                        savedDocument.model->gff().dirty(wasDirty);
                        frame->updateDocumentTabTitle(savedDocument);
                        if (index == frame->activeDocumentIndex_) frame->refreshAll();
                        wxui::showMessage(
                            frame,
                            "Replacement download ready",
                            "The browser could not overwrite the original host file directly. "
                            "A replacement GFF resource is ready in the download panel. The document remains marked modified. "
                            "Download the replacement, then repeat the navigation and explicitly discard the in-memory copy.");
                        if (completion) completion(false);
                        return;
                    }

                    if (!result.saved()) {
                        savedDocument.model->gff().dirty(wasDirty);
                        frame->updateDocumentTabTitle(savedDocument);
                        if (index == frame->activeDocumentIndex_) frame->refreshAll();
                        wxMessageBox(
                            "The browser did not confirm that the GFF resource was written.",
                            "Save Failed", wxOK | wxICON_ERROR, frame);
                        if (completion) completion(false);
                        return;
                    }

                    savedDocument.logicalFilename = target;
                    savedDocument.model->gff().dirty(false);
                    if (!frame->importOwnsPath(savedDocument.sourceImport, target)) {
                        savedDocument.sourceImport.reset();
                    }
                    frame->rememberRecentFile(target);
                    neogames::resolver().inferFromOpenedPath(target);
                    frame->updateDocumentTabTitle(savedDocument);
                    if (index == frame->activeDocumentIndex_) frame->refreshAll();
                    if (completion) completion(true);
                });
            return true;
#else
            document.logicalFilename = target;
            document.model->gff().dirty(false);
            rememberRecentFile(target);
            neogames::resolver().inferFromOpenedPath(target);
            refreshAll();
            if (completion) completion(true);
            return true;
#endif
        } catch (const std::exception& ex) {
            if (!forcedTarget.empty()) throw;
            wxui::showError(this, ex);
            if (completion) completion(false);
            return false;
        }
    }

    int selectedGridRow() const {
        return viewState().selectedLogicalRow;
    }

    std::string selectedContainerPath() const {
        const int row = selectedGridRow();
        if (row < 0 || row >= static_cast<int>(displayRows_.size())) {
            return viewState().selectedPath;
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



    void materializeTreeChildren(const wxTreeItemId& parentItem,
                                 const std::string& parentPath) {
        if (!tree_ || !parentItem.IsOk()) return;
        if (!treeMaterializedPaths_.insert(parentPath).second) return;

        const auto children = treeChildrenByParent_.find(parentPath);
        if (children == treeChildrenByParent_.end()) return;

        for (const std::size_t rowIndex : children->second) {
            if (rowIndex >= displayRows_.size()) continue;
            const auto& row = displayRows_[rowIndex];
            const wxTreeItemId item = tree_->AppendItem(
                parentItem,
                wxui::toWx(treeTextForRow(row)),
                -1,
                -1,
                new GffTreeItemData(row.path, static_cast<int>(rowIndex)));
            treeRowItems_[rowIndex] = item;
            treeItemsByPath_[row.path] = item;

            const auto grandchildren = treeChildrenByParent_.find(row.path);
            if (grandchildren != treeChildrenByParent_.end() &&
                !grandchildren->second.empty()) {
                tree_->SetItemHasChildren(item, true);
            }
        }
    }

    void onTreeItemExpanding(wxTreeEvent& event) {
        if (!tree_) {
            event.Skip();
            return;
        }
        const wxTreeItemId item = event.GetItem();
        if (!item.IsOk()) {
            event.Skip();
            return;
        }

        std::string path;
        if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
            path = data->path();
        }
        materializeTreeChildren(item, path);
        event.Skip();
    }

    wxTreeItemId ensureTreeItemForKey(const std::string& key) {
        if (!tree_) return {};
        const wxTreeItemId root = tree_->GetRootItem();
        if (key == "$root") return root;

        const auto existing = treeItemsByPath_.find(key);
        if (existing != treeItemsByPath_.end() && existing->second.IsOk()) {
            return existing->second;
        }

        const std::string parentPath = treeParentPathOf(key);
        const wxTreeItemId parent = parentPath.empty()
            ? root
            : ensureTreeItemForKey(parentPath);
        if (!parent.IsOk()) return {};
        materializeTreeChildren(parent, parentPath);

        const auto created = treeItemsByPath_.find(key);
        return created == treeItemsByPath_.end() ? wxTreeItemId{} : created->second;
    }

    void selectDefaultTreeItem() {
        const auto rootRows = treeChildrenByParent_.find(std::string{});
        if (rootRows == treeChildrenByParent_.end() || rootRows->second.empty()) return;
        const std::size_t firstRow = rootRows->second.front();
        if (firstRow >= treeRowItems_.size() || !treeRowItems_[firstRow].IsOk()) return;
        tree_->SelectItem(treeRowItems_[firstRow]);
        updateSelectionStateFromTreeItem(treeRowItems_[firstRow]);
    }

    void refreshTree() {
        if (!tree_ || !hasActiveDocument()) return;

        if (treeRenderedDocumentPage_ == activeDocument().tabPage) {
            captureRenderedTreeState();
        }

        wxWindowUpdateLocker updateLocker(tree_);
        treeRefreshInProgress_ = true;
        treeRowItems_.assign(displayRows_.size(), wxTreeItemId{});
        treeItemsByPath_.clear();
        treeChildrenByParent_.clear();
        treeMaterializedPaths_.clear();
        tree_->DeleteAllItems();

        const std::filesystem::path currentFilename = documentFilename(activeDocument());
        const std::string rootText = model().loaded()
            ? ((currentFilename.empty() && !activeDocument().sourceDescription.empty() ? activeDocument().sourceDescription : pathText(currentFilename)).empty() ? std::string("New GFF File") : (currentFilename.empty() && !activeDocument().sourceDescription.empty() ? activeDocument().sourceDescription : pathText(currentFilename)))
            : std::string("No GFF loaded");
        const wxTreeItemId root = tree_->AddRoot(
            wxui::toWx(rootText), -1, -1, new GffTreeItemData(std::string{}, -1));

        if (model().loaded() && !displayRows_.empty()) {
            std::unordered_map<std::string, std::size_t> visibleRowsByPath;
            visibleRowsByPath.reserve(displayRows_.size());
            for (std::size_t i = 0; i < displayRows_.size(); ++i) {
                visibleRowsByPath.emplace(displayRows_[i].path, i);
            }

            for (std::size_t i = 0; i < displayRows_.size(); ++i) {
                std::string parentPath = treeParentPathOf(displayRows_[i].path);
                while (!parentPath.empty() &&
                       visibleRowsByPath.find(parentPath) == visibleRowsByPath.end()) {
                    parentPath = treeParentPathOf(parentPath);
                }
                treeChildrenByParent_[parentPath].push_back(i);
            }

            materializeTreeChildren(root, std::string{});
        }

        const neotree::TreeViewState& state = activeDocument().treeState;
        if (state.initialized) {
            const neotree::TreeRestoreResult restored = neotree::restoreTreeViewState(
                *tree_, state,
                [this](const std::string& key) { return ensureTreeItemForKey(key); });
            if (restored.selectionRestored) {
                updateSelectionStateFromTreeItem(tree_->GetSelection());
            } else if (!viewState().selectedPath.empty()) {
                const wxTreeItemId selected = ensureTreeItemForKey(viewState().selectedPath);
                if (selected.IsOk()) {
                    tree_->SelectItem(selected);
                    updateSelectionStateFromTreeItem(selected);
                } else {
                    selectDefaultTreeItem();
                }
            } else {
                selectDefaultTreeItem();
            }
        } else {
            tree_->Expand(root);
            selectDefaultTreeItem();
        }

        treeRenderedDocumentPage_ = activeDocument().tabPage;
        treeRefreshInProgress_ = false;
    }

    void expandTreeRecursive(const wxTreeItemId& item) {
        if (!tree_ || !item.IsOk()) return;
        std::string path;
        if (auto* data = dynamic_cast<GffTreeItemData*>(tree_->GetItemData(item))) {
            path = data->path();
        }
        materializeTreeChildren(item, path);
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

    bool gffRowPassesCurrentFilters(const GffFieldRow& row) const {
        if (viewState().filterTerm.empty()) return true;
        for (std::size_t column = 0; column < kColumnCount; ++column) {
            if (neoview::containsInsensitive(gffCellText(row, column), viewState().filterTerm)) return true;
        }
        return false;
    }

    void refreshActiveView() {
        refreshTree();
    }

    void refreshAll() {
        std::vector<GffFieldRow> modelRows;
        if (model().loaded()) modelRows = model().rows();
        const std::size_t totalRows = modelRows.size();

        displayRows_.clear();
        displayRows_.reserve(modelRows.size());
        neoview::removeColumnFiltersOutsideRange(viewState(), kColumnCount);

        std::vector<std::size_t> visibleLogicalRows;
        visibleLogicalRows.reserve(modelRows.size());
        for (std::size_t i = 0; i < modelRows.size(); ++i) {
            if (gffRowPassesCurrentFilters(modelRows[i])) {
                displayRows_.push_back(std::move(modelRows[i]));
                visibleLogicalRows.push_back(i);
            }
        }

        neoview::setRowsFromLogicalRows(
            viewState(), std::move(visibleLogicalRows));
        neoview::ensureIdentityColumns(viewState(), kColumnCount);

        const std::filesystem::path currentFilename = documentFilename(activeDocument());
        filePath_->ChangeValue(wxui::toWx((currentFilename.empty() && !activeDocument().sourceDescription.empty() ? activeDocument().sourceDescription : pathText(currentFilename))));
        typeText_->ChangeValue(wxui::toWx(typeVersionText(model())));
        if (tlkPath_) {
            tlkPath_->ChangeValue(wxui::toWx(
                model().tlk().loaded()
                    ? pathText(model().tlk().filename())
                    : std::string{}));
        }

        setModuleTitle(wxui::toWx(
            std::string("NeoGFF v") + kVersion +
            std::string(activeDocument().saveInProgress ? " (saving...)" :
                        (model().dirty() ? " *" : "")) +
            " (GFF editor)"));
        updateActiveTabTitle();

        refreshActiveView();

        if (moduleStatusBar()) {
            setModuleStatusText(wxui::toWx(
                    model().loaded()
                        ? (currentFilename.empty() && !activeDocument().sourceDescription.empty() ? activeDocument().sourceDescription : pathText(currentFilename))
                        : std::string("No GFF loaded")),
                0);
            std::string detail =
                std::to_string(displayRows_.size()) + "/" +
                std::to_string(totalRows) + " rows";
            if (model().tlk().loaded()) {
                detail += "; TLK " +
                          std::to_string(model().tlk().count()) +
                          " entries";
            } else if (!tlkAutoLoadWarning().empty()) {
                detail += "; " + tlkAutoLoadWarning();
            }
            setModuleStatusText(wxui::toWx(detail), 1);
        }
    }

    void applyDarkMode() {
        if (darkModeItem_) darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this, darkMode_);
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
    bool browserSaveActive_ = false;
    std::vector<GffFieldRow> displayRows_;
    wxTextCtrl* filePath_ = nullptr;
    wxTextCtrl* typeText_ = nullptr;
    wxTextCtrl* tlkPath_ = nullptr;
    wxTextCtrl* filterText_ = nullptr;
    wxPanel* viewPanel_ = nullptr;
    wxBoxSizer* viewSizer_ = nullptr;
    wxTreeCtrl* tree_ = nullptr;
    std::vector<wxTreeItemId> treeRowItems_;
    std::unordered_map<std::string, wxTreeItemId> treeItemsByPath_;
    std::unordered_map<std::string, std::vector<std::size_t>> treeChildrenByParent_;
    std::unordered_set<std::string> treeMaterializedPaths_;
    wxWindow* treeRenderedDocumentPage_ = nullptr;
    bool treeRefreshInProgress_ = false;
    wxMenuItem* darkModeItem_ = nullptr;
    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};


} // namespace

namespace neogff::ui {
GFFEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new NeoGFFPanelImpl(parent, std::move(context));
}
}
