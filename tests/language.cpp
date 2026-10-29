#include "game/text/LanguageDb.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace fut;
void Require(bool ok) { if (!ok) throw std::runtime_error("language regression"); }

int main() {
    using text::TextManager;
    Require(std::string(TextManager::ParseLanguage("gErMaN")) == "German");
    for (const char* name : {"English", "French", "Italian", "Spanish"})
        Require(std::string(TextManager::ParseLanguage(name)) == name);
    Require(!TextManager::ParseLanguage("Deutsch"));
    Require(!TextManager::ParseLanguage(""));
    Require(!TextManager::ParseLanguage("German "));
    // Synthetic bilingual LDB: no retail data is required or embedded.
    std::vector<u8> file(0x1f0, 0);
    auto word = [&](size_t at, u32 value) {
        for (unsigned i = 0; i < 4; ++i) file[at + i] = value >> (8 * i);
    };
    std::memcpy(file.data(), "0BDL", 4);
    word(4, 1); word(8, 0x1f0); word(12, 9);
    for (char c : {'h','e','l','l','o','\0','\0','\0','\0'}) file.push_back(c);
    auto language = [&](size_t slot, const char* name, std::u16string value) {
        const size_t header = 0x10 + slot * 0x20;
        word(header, file.size()); word(header + 4, (value.size() + 1) * 2);
        std::memcpy(file.data() + header + 8, name, std::strlen(name));
        value.push_back(0);
        for (char16_t c : value) { file.push_back(c & 255); file.push_back(c >> 8); }
    };
    language(0, "English", u"Hello");
    language(1, "German", u"Grüße");
    TextManager english({});
    auto* original = english.LoadDbFromMemory(file, "synthetic.ldb");
    Require(original && std::u16string(english.GetText("hello", original)) == u"Hello");
    TextManager german([&](const std::string&, std::vector<u8>& out) { out = file; return true; });
    german.SetLanguage("German");
    auto* selected = german.LoadDb("synthetic.ldb");
    Require(selected && std::u16string(german.GetText("hello", selected)) == u"Grüße");
    german.SetLanguage("English");
    Require(std::u16string(german.GetText("hello", selected)) == u"Hello");
    std::cout << "language validation, default, early selection and Unicode passed\n";
}
