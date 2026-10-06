#include "SessionFolderNaming.h"
#include "NfcCompositions.h"
#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>
#include <cctype>
#include <sstream>
#include <iomanip>

namespace mma {

namespace {

// ---- UTF-8 ---------------------------------------------------------------

std::vector<char32_t> decodeUtf8 (const std::string& text)
{
    std::vector<char32_t> out;
    out.reserve (text.size());

    for (size_t i = 0; i < text.size();)
    {
        const auto lead = static_cast<unsigned char> (text[i]);
        int length = 0;
        char32_t cp = 0;

        if (lead < 0x80)                { cp = lead;         length = 1; }
        else if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1Fu; length = 2; }
        else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0Fu; length = 3; }
        else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07u; length = 4; }
        else                            { ++i; continue; } // stray byte: dropped

        if (i + static_cast<size_t> (length) > text.size())
            break;

        bool valid = true;
        for (int k = 1; k < length; ++k)
        {
            const auto next = static_cast<unsigned char> (text[i + static_cast<size_t> (k)]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (next & 0x3Fu);
        }

        // Overlong forms, surrogates and out-of-range values are not text.
        static constexpr char32_t kSmallest[] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (! valid || cp < kSmallest[length] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        {
            ++i;
            continue;
        }

        out.push_back (cp);
        i += static_cast<size_t> (length);
    }

    return out;
}

void appendUtf8 (std::string& out, char32_t cp)
{
    if (cp < 0x80)
    {
        out += static_cast<char> (cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char> (0xC0 | (cp >> 6));
        out += static_cast<char> (0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char> (0xE0 | (cp >> 12));
        out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char> (0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char> (0xF0 | (cp >> 18));
        out += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char> (0x80 | (cp & 0x3F));
    }
}

// ---- NFC -----------------------------------------------------------------

bool composePair (char32_t base, char32_t mark, char32_t& composed)
{
    // Hangul, algorithmically: L+V -> LV, LV+T -> LVT. A Korean name in a
    // file name macOS handed back decomposed arrives as separate jamo.
    constexpr char32_t sBase = 0xAC00, lBase = 0x1100, vBase = 0x1161, tBase = 0x11A7;
    constexpr char32_t lCount = 19, vCount = 21, tCount = 28, nCount = vCount * tCount;

    if (base >= lBase && base < lBase + lCount && mark >= vBase && mark < vBase + vCount)
    {
        composed = sBase + ((base - lBase) * vCount + (mark - vBase)) * tCount;
        return true;
    }

    if (base >= sBase && base < sBase + lCount * nCount && (base - sBase) % tCount == 0
        && mark > tBase && mark < tBase + tCount)
    {
        composed = base + (mark - tBase);
        return true;
    }

    const auto* first = std::begin (nfc::kCompositions);
    const auto* last = std::end (nfc::kCompositions);
    const auto* found = std::lower_bound (first, last, std::make_pair (base, mark),
                                          [] (const nfc::Composition& c, const std::pair<char32_t, char32_t>& key)
                                          {
                                              return c.base != key.first ? c.base < key.first
                                                                         : c.mark < key.second;
                                          });

    if (found == last || found->base != base || found->mark != mark)
        return false;

    composed = found->composed;
    return true;
}

/// Canonical composition of a decomposed sequence, mark by mark onto the
/// character before it ("e" + U+0323 + U+0302 -> U+1EC7). Enough to make a
/// name that arrived decomposed -- macOS file names often do -- come out the
/// same bytes as one that was typed, which is what the folder name needs.
std::vector<char32_t> composeNfc (const std::vector<char32_t>& in)
{
    std::vector<char32_t> out;
    out.reserve (in.size());

    for (const auto cp : in)
    {
        char32_t composed = 0;
        if (! out.empty() && composePair (out.back(), cp, composed))
            out.back() = composed;
        else
            out.push_back (cp);
    }

    return out;
}

// ---- What a name may hold ---------------------------------------------------

bool isUnicodeSpace (char32_t cp)
{
    return cp == 0x00A0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A)
        || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/// Non-ASCII code points a file name must not carry: C1 controls, invisible
/// format and direction marks (a right-to-left override in a file name makes
/// Finder show a different name from the real one), byte-order marks,
/// private-use and noncharacters.
bool isUnwantedNonAscii (char32_t cp)
{
    return (cp >= 0x0080 && cp <= 0x009F)
        || cp == 0x00AD
        || (cp >= 0x200B && cp <= 0x200F)
        || (cp >= 0x202A && cp <= 0x202E)
        || (cp >= 0x2060 && cp <= 0x206F)
        || cp == 0xFEFF
        || (cp >= 0xE000 && cp <= 0xF8FF)
        || (cp >= 0xFDD0 && cp <= 0xFDEF)
        || (cp & 0xFFFE) == 0xFFFE
        || (cp >= 0xFFF0 && cp <= 0xFFFF)
        || cp >= 0xF0000;
}

/// The sanitized name, or empty when nothing usable was left.
///
/// ASCII is held to the original §6.2 set -- letters, digits, '_' and '-' --
/// which already leaves out everything Windows, FAT and Finder object to
/// (/ \ : * ? " < > | and control characters) and the dots that hide a file
/// or vanish from the end of one. Letters beyond ASCII are kept: they used to
/// be dropped byte by byte, so "Müller" became "Mller" and a name in Japanese
/// or Greek became "Session". Whitespace of any kind still collapses to one
/// hyphen. Composed to NFC first so the same name always gives the same bytes.
/// The length limit counts characters, not bytes, so a name is never cut in
/// the middle of one.
std::string sanitizeToCore (const std::string& rawName, size_t maxCodePoints)
{
    std::string out;
    size_t kept = 0;
    bool inWhitespaceRun = false;

    const auto emit = [&] (char32_t cp) -> bool
    {
        if (kept >= maxCodePoints)
            return false;

        appendUtf8 (out, cp);
        ++kept;
        return true;
    };

    for (const auto cp : composeNfc (decodeUtf8 (rawName)))
    {
        if (cp == ' ' || isUnicodeSpace (cp))
        {
            inWhitespaceRun = true;
            continue;
        }

        const bool keep = cp < 0x80
            ? (std::isalnum (static_cast<unsigned char> (cp)) || cp == '_' || cp == '-')
            : ! isUnwantedNonAscii (cp);

        if (! keep)
            continue;

        if (inWhitespaceRun)
        {
            inWhitespaceRun = false;
            if (! out.empty() && ! emit ('-'))
                break;
        }

        if (! emit (cp))
            break;
    }

    if (inWhitespaceRun && ! out.empty())
        emit ('-');

    return out;
}

} // namespace

std::string SessionFolderNaming::sanitizeName (const std::string& rawName)
{
    auto collapsed = sanitizeToCore (rawName, kMaxNameLength);

    if (collapsed.empty())
        collapsed = kDefaultName;

    return collapsed;
}

std::string SessionFolderNaming::sanitizeNameOrEmpty (const std::string& rawName)
{
    const auto isSpace = [] (char c) { return std::isspace (static_cast<unsigned char> (c)) != 0; };

    auto first = rawName.begin();
    auto last = rawName.end();
    while (first != last && isSpace (*first))       ++first;
    while (last != first && isSpace (*(last - 1)))  --last;

    const std::string trimmed (first, last);

    // Nothing but separators is not a name either: "-" would become the strip,
    // the stem and the movie just as "Session" did. A letter in any script
    // counts -- "山田" is a name, and used to be thrown away as no name at all.
    const auto core = sanitizeToCore (trimmed, kMaxNameLength);
    const bool hasLetterOrDigit = std::any_of (core.begin(), core.end(), [] (char c)
    {
        return c != '-' && c != '_';
    });

    if (! hasLetterOrDigit)
        return {};

    return sanitizeName (trimmed);
}

std::string SessionFolderNaming::foldCaseForComparison (const std::string& utf8Name)
{
    std::string out;
    out.reserve (utf8Name.size());

    for (auto cp : composeNfc (decodeUtf8 (utf8Name)))
    {
        if (cp >= 'A' && cp <= 'Z')
            cp += 0x20;
        else if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)                       // Latin-1
            cp += 0x20;
        else if ((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) // Latin Ext-A, even = capital
            cp |= 1u;
        else if (cp >= 0x139 && cp <= 0x148 && (cp & 1u) != 0)                 // Latin Ext-A, odd = capital
            ++cp;
        else if (cp >= 0x179 && cp <= 0x17E && (cp & 1u) != 0)
            ++cp;
        else if (cp == 0x178)                                                  // Ÿ
            cp = 0xFF;
        else if (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2)                    // Greek
            cp += 0x20;
        else if (cp >= 0x410 && cp <= 0x42F)                                   // Cyrillic
            cp += 0x20;
        else if (cp >= 0x400 && cp <= 0x40F)
            cp += 0x50;

        appendUtf8 (out, cp);
    }

    return out;
}

std::string SessionFolderNaming::buildFolderName (int year, int month, int day, int hour, int minute,
                                                  const std::string& sanitizedName)
{
    std::ostringstream oss;
    oss << std::setfill ('0')
        << std::setw (4) << year << '-'
        << std::setw (2) << month << '-'
        << std::setw (2) << day << '_'
        << std::setw (2) << hour
        << std::setw (2) << minute << '_'
        << sanitizedName;
    return oss.str();
}

std::string SessionFolderNaming::resolveCollision (const std::string& desiredFolderName,
                                                   const std::function<bool (const std::string&)>& exists)
{
    if (! exists (desiredFolderName))
        return desiredFolderName;

    int suffix = 2;
    while (true)
    {
        std::string candidate = desiredFolderName + "_" + std::to_string (suffix);
        if (! exists (candidate))
            return candidate;
        ++suffix;
    }
}

} // namespace mma
