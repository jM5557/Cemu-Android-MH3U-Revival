// Cheat file parsing and writing. Kept free of emulator dependencies so it can be tested on its own.
#include "Cafe/Cheats/CheatManager.h"

#include <sstream>

namespace CheatManager
{
	static std::string Trim(const std::string& s)
	{
		size_t b = 0, e = s.size();
		while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
			b++;
		while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
			e--;
		return s.substr(b, e - b);
	}

	std::vector<Cheat> Parse(const std::string& text)
	{
		std::vector<Cheat> cheats;
		std::istringstream in(text);
		std::string raw;
		bool inNotes = false;
		while (std::getline(in, raw))
		{
			std::string line = Trim(raw);

			if (inNotes)
			{
				const size_t close = line.find('}');
				std::string part = (close == std::string::npos) ? line : line.substr(0, close);
				if (!cheats.empty())
				{
					cheats.back().notes += '\n';
					cheats.back().notes += part;
				}
				if (close != std::string::npos)
					inNotes = false;
				continue;
			}

			if (line.empty())
				continue;

			if (line.front() == '[')
			{
				const size_t close = line.rfind(']');
				Cheat c;
				c.name = Trim(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
				cheats.push_back(std::move(c));
				continue;
			}

			// anything before the first [name] header has nowhere to go
			if (cheats.empty())
				continue;

			if (line.front() == '{')
			{
				const size_t close = line.find('}');
				if (close == std::string::npos)
				{
					cheats.back().notes = line.substr(1);
					inNotes = true;
				}
				else
					cheats.back().notes = line.substr(1, close - 1);
				continue;
			}

			if (line == "*cemu_enabled" || line == "*citra_enabled")
			{
				cheats.back().enabled = true;
				continue;
			}

			cheats.back().code.push_back(line);
		}
		return cheats;
	}

	std::string Serialize(const std::vector<Cheat>& cheats)
	{
		std::string out;
		for (const auto& c : cheats)
		{
			out += '[';
			out += c.name;
			out += "]\n";
			if (!c.notes.empty())
			{
				// a '}' inside the notes would end them early on the next load
				std::string notes = c.notes;
				for (auto& ch : notes)
					if (ch == '}')
						ch = ')';
				out += '{';
				out += notes;
				out += "}\n";
			}
			if (c.enabled)
				out += "*cemu_enabled\n";
			for (const auto& l : c.code)
			{
				std::string t = Trim(l);
				if (t.empty())
					continue;
				out += t;
				out += '\n';
			}
			out += '\n';
		}
		return out;
	}
}
