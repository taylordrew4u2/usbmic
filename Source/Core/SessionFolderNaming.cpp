#include "SessionFolderNaming.h"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace mma {

std::string SessionFolderNaming::sanitizeName (const std::string& rawName)
{
    std::string allowed;
    allowed.reserve (rawName.size());
    for (char c : rawName)
    {
        if (std::isalnum (static_cast<unsigned char> (c)) || c == ' ' || c == '_' || c == '-')
            allowed += c;
    }

    // Collapse runs of whitespace to a single hyphen.
    std::string collapsed;
    collapsed.reserve (allowed.size());
    bool inWhitespaceRun = false;
    for (char c : allowed)
    {
        if (c == ' ')
        {
            inWhitespaceRun = true;
        }
        else
        {
            if (inWhitespaceRun)
            {
                collapsed += '-';
                inWhitespaceRun = false;
            }
            collapsed += c;
        }
    }
    if (inWhitespaceRun && ! collapsed.empty())
        collapsed += '-';

    if (collapsed.size() > kMaxNameLength)
        collapsed.resize (kMaxNameLength);

    if (collapsed.empty())
        collapsed = kDefaultName;

    return collapsed;
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
