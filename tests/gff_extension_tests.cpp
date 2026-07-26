#include "core/AppModel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path uniqueTestDirectory() {
    const auto stamp = std::chrono::high_resolution_clock::now()
                           .time_since_epoch()
                           .count();
    return std::filesystem::temp_directory_path() /
           ("neogff-jade-extension-tests-" + std::to_string(stamp));
}

void testJadeGffOpenSaveAs() {
    struct ResourceType {
        const char* extension;
        const char* fileType;
    };

    const ResourceType resources[] = {
        {"qst", "QST "},
        {"qst2", "QST "},
        {"pla", "PLA "},
        {"cre", "CRE "},
        {"trg", "TRG "},
        {"dlg", "DLG "},
        {"fsm", "FSM "},
        {"gff", "GFF "},
        {"are", "ARE "},
        {"gui", "GUI "},
        {"sto", "STO "},
        {"cwa", "CWA "},
        {"cwd", "CWD "},
        {"sav", "SAV "},
    };

    const ResourceType otherClassicResources[] = {
        {"cam", "UTW "},
        {"uta", "UTA "},
        {"utx", "UTX "},
        {"gic", "GIC "},
        {"mmd", "MMD "},
    };

    const std::filesystem::path root = uniqueTestDirectory();
    std::filesystem::create_directories(root);

    try {
        const auto& jadeExtensions = neogff::jadeEmpireGffResourceExtensions();
        for (const ResourceType& resource : resources) {
            require(neogff::isKnownGffResourceExtension(resource.extension),
                    std::string("NeoGFF does not recognize .") + resource.extension);
            std::string upperExtension = resource.extension;
            std::transform(upperExtension.begin(), upperExtension.end(), upperExtension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            require(neogff::isKnownGffResourceExtension("." + upperExtension),
                    std::string("NeoGFF does not recognize uppercase .") + upperExtension);
            require(std::find(jadeExtensions.begin(), jadeExtensions.end(),
                              resource.extension) != jadeExtensions.end(),
                    std::string("Jade filter does not include .") + resource.extension);

            neogff::GffModel created;
            created.newFile(resource.fileType);
            const std::filesystem::path original =
                root / (std::string("original.") + resource.extension);
            created.save(original);

            neogff::GffModel opened;
            opened.load(original);
            require(opened.fileType() == resource.fileType,
                    std::string("Opening .") + resource.extension +
                        " changed its GFF content type");

            const std::filesystem::path savedAs =
                root / (std::string("saved-as.") + resource.extension);
            opened.save(savedAs);

            neogff::GffModel reopened;
            reopened.load(savedAs);
            require(reopened.fileType() == resource.fileType,
                    std::string("Save As for .") + resource.extension +
                        " changed its GFF content type");
        }

        for (const ResourceType& resource : otherClassicResources) {
            require(neogff::isKnownGffResourceExtension(resource.extension),
                    std::string("NeoGFF does not recognize .") + resource.extension);
            std::string upperExtension = resource.extension;
            std::transform(upperExtension.begin(), upperExtension.end(), upperExtension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            require(neogff::isKnownGffResourceExtension("." + upperExtension),
                    std::string("NeoGFF does not recognize uppercase .") + upperExtension);

            neogff::GffModel created;
            created.newFile(resource.fileType);
            const std::filesystem::path original =
                root / (std::string("classic-original.") + resource.extension);
            created.save(original);

            neogff::GffModel opened;
            opened.load(original);
            require(opened.fileType() == resource.fileType,
                    std::string("Opening .") + resource.extension +
                        " changed its GFF content type");

            const std::filesystem::path savedAs =
                root / (std::string("classic-saved-as.") + resource.extension);
            opened.save(savedAs);

            neogff::GffModel reopened;
            reopened.load(savedAs);
            require(reopened.fileType() == resource.fileType,
                    std::string("Save As for .") + resource.extension +
                        " changed its GFF content type");
        }

        const auto& dragonAgeExtensions = neogff::dragonAgeGff4ResourceExtensions();
        for (const char* extension : {"gad", "rml", "anb", "tnt"}) {
            require(std::find(dragonAgeExtensions.begin(), dragonAgeExtensions.end(), extension) !=
                        dragonAgeExtensions.end(),
                    std::string("Dragon Age GFF4 filter does not include .") + extension);
            std::string upperExtension = extension;
            std::transform(upperExtension.begin(), upperExtension.end(), upperExtension.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            require(neogff::isKnownDragonAgeGff4ResourceExtension("." + upperExtension),
                    std::string("Dragon Age GFF4 registry does not recognize uppercase .") +
                        upperExtension);
        }

        neogff::GffModel v33;
        v33.newFile("UTC ");
        v33.gff().version("V3.3");
        const std::filesystem::path v33Path = root / "witcher-v33.utc";
        v33.save(v33Path);
        neogff::GffModel reopenedV33;
        reopenedV33.load(v33Path);
        require(reopenedV33.version() == "V3.3",
                "NeoGFF did not preserve a classic GFF V3.3 document");

        neogff::GffModel witcherMmd;
        witcherMmd.newFile("MMD ");
        witcherMmd.gff().version("V3.3");
        const std::filesystem::path mmdPath = root / "witcher-module.mmd";
        witcherMmd.save(mmdPath);
        neogff::GffModel reopenedMmd;
        reopenedMmd.load(mmdPath);
        require(reopenedMmd.fileType() == "MMD " && reopenedMmd.version() == "V3.3",
                "NeoGFF did not preserve a Witcher MMD V3.3 document");

        for (const char* nonGff : {"art", "lyt", "vis", "bip", "amp", "ndb",
                                   "wfx", "rim", "trx"}) {
            require(!neogff::isKnownGffResourceExtension(nonGff),
                    std::string("NeoGFF incorrectly classified .") + nonGff +
                        " as GFF");
        }

        neogff::GffModel duplicateModel;
        duplicateModel.newFile("SAV ");
        neogff::GffStruct* duplicateRoot = duplicateModel.gff().root();
        require(duplicateRoot != nullptr, "Synthetic SAV root is missing");
        duplicateRoot->AddField(std::make_unique<neogff::GffIntField>("Repeated", 10));
        duplicateRoot->AddField(std::make_unique<neogff::GffIntField>("Repeated", 20));
        const std::filesystem::path duplicatePath = root / "duplicate-labels.sav";
        duplicateModel.save(duplicatePath);

        neogff::GffModel reopenedDuplicates;
        reopenedDuplicates.load(duplicatePath);
        const auto duplicateRows = reopenedDuplicates.rows();
        const auto first = std::find_if(duplicateRows.begin(), duplicateRows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#1]";
        });
        const auto second = std::find_if(duplicateRows.begin(), duplicateRows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#2]";
        });
        require(first != duplicateRows.end() && first->value == "10", "First duplicate field occurrence was not exposed");
        require(second != duplicateRows.end() && second->value == "20", "Second duplicate field occurrence was not exposed");
        reopenedDuplicates.setValue("Repeated[#2]", "25");
        reopenedDuplicates.save(duplicatePath);

        neogff::GffModel editedDuplicates;
        editedDuplicates.load(duplicatePath);
        const auto editedRows = editedDuplicates.rows();
        const auto editedSecond = std::find_if(editedRows.begin(), editedRows.end(), [](const neogff::GffFieldRow& row) {
            return row.path == "Repeated[#2]";
        });
        require(editedSecond != editedRows.end() && editedSecond->value == "25",
                "Occurrence-aware duplicate field edit did not round-trip");
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        throw;
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    try {
        testJadeGffOpenSaveAs();
        std::cout << "NeoGFF Jade extension tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NeoGFF Jade extension test failure: " << error.what() << '\n';
        return 1;
    }
}
