#include "core/AppModel.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: large-dlg-fixture <output.dlg>\n";
        return 2;
    }

    try {
        neogff::GffModel model;
        model.newFile("DLG");

        neogff::GffStruct* root = model.gff().root();
        if (root == nullptr) {
            throw std::runtime_error("new DLG has no root structure");
        }

        constexpr std::size_t kEntryCount = 12000;
        auto entries = std::make_unique<neogff::GffList>("EntryList");
        for (std::size_t i = 0; i < kEntryCount; ++i) {
            auto entry = std::make_unique<neogff::GffStruct>();
            entry->typeidValue(0);
            entry->AddField(std::make_unique<neogff::GffExoStringField>(
                "Speaker", "SyntheticSpeaker"));
            entry->AddField(std::make_unique<neogff::GffLocalizedStringField>(
                "Text", static_cast<neogff::UInt32>(100000u + i)));
            entry->AddField(std::make_unique<neogff::GffIntField>(
                "NodeID", static_cast<std::int32_t>(i)));
            entry->AddField(std::make_unique<neogff::GffExoStringField>(
                "Comment", "Large-dialogue GUI regression fixture"));
            entry->AddField(std::make_unique<neogff::GffList>("RepliesList"));
            entries->AddStruct(std::move(entry));
        }
        root->AddField(std::move(entries));

        const std::filesystem::path output(argv[1]);
        model.save(output);

        const std::size_t rowCount = model.rows().size();
        if (rowCount < 70000) {
            std::cerr << "fixture is unexpectedly small: " << rowCount
                      << " rows\n";
            return 3;
        }
        std::cout << output.string() << ": " << rowCount << " rows\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
