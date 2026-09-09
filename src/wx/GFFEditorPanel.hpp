#pragma once
#include "NeoModulePanel.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <gff/AppModel.hpp>
namespace neogff::ui {
inline constexpr unsigned kEditorApiVersion=1;
class GFFEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path)=0;
    virtual bool openResource(neoshared::ResourceDocument resource)=0;
    virtual bool saveActiveAs(const std::filesystem::path& path)=0;
    virtual bool activateResource(const std::string& identity)=0;
    virtual std::size_t documentCount() const=0;
    virtual neogff::GffModel* activeModel()=0;
    virtual void refreshActiveDocument()=0;
};
GFFEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context={});
} // namespace neogff::ui
