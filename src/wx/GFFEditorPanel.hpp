#pragma once
#include "NeoModulePanel.hpp"
#include <neoshared/ResourceDocument.hpp>
#include <gff/AppModel.hpp>
namespace neogff::ui {
inline constexpr unsigned kEditorApiVersion=4;
wxDECLARE_EVENT(EVT_GFF_DOCUMENT_EDITED, wxCommandEvent);
wxDECLARE_EVENT(EVT_GFF_DOCUMENT_CHANGING, wxNotifyEvent);
wxDECLARE_EVENT(EVT_GFF_DOCUMENT_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_GFF_FIELD_SELECTED, wxCommandEvent);
class GFFEditorPanel : public neomodules::Panel {
public:
    using Panel::Panel;
    virtual bool openFile(const std::filesystem::path& path)=0;
    virtual neoshared::ResourceResolverFactory resourceResolverFactory() const=0;
    virtual bool openResource(neoshared::ResourceDocument resource,neoshared::ResourceResolverFactory resources={})=0;
    virtual bool saveActiveAs(const std::filesystem::path& path)=0;
    virtual bool activateResource(const std::string& identity)=0;
    virtual std::size_t documentCount() const=0;
    virtual wxWindow* moveDocumentTabs(wxWindow* parent)=0;
    virtual neogff::GffModel* activeModel()=0;
    virtual std::filesystem::path activeFilename() const=0;
    virtual std::string activeResourceIdentity() const=0;
    virtual void selectField(const std::string& path)=0;
    virtual void clearDocuments()=0;
    virtual void refreshActiveDocument()=0;
};
GFFEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context={}, bool managedDocuments=false);
} // namespace neogff::ui
