#include "wxgui/CheatsWindow.h"

#include "wxgui/wxgui.h"
#include "wxgui/wxHelper.h"

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <sstream>

enum
{
	ID_CHEATS_LIST = wxID_HIGHEST + 1,
	ID_CHEATS_NAME,
	ID_CHEATS_NOTES,
	ID_CHEATS_CODE,
	ID_CHEATS_ADD,
	ID_CHEATS_DELETE,
	ID_CHEATS_SAVE,
	ID_CHEATS_RELOAD,
	ID_CHEATS_OPENFOLDER,
};

static std::string ToUtf8(const wxString& s)
{
	return std::string(s.utf8_str());
}

CheatsWindow::CheatsWindow(wxWindow* parent, uint64 titleId, const wxString& gameName)
	: wxDialog(parent, wxID_ANY, _("Cheats"), wxDefaultPosition, wxSize(760, 520), wxCLOSE_BOX | wxCLIP_CHILDREN | wxCAPTION | wxRESIZE_BORDER),
	  m_titleId(titleId)
{
	auto* sizer = new wxBoxSizer(wxVERTICAL);

	auto* heading = new wxStaticText(this, wxID_ANY, gameName);
	wxFont headingFont = heading->GetFont();
	headingFont.MakeBold();
	heading->SetFont(headingFont);
	sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, 10);

	auto* body = new wxBoxSizer(wxHORIZONTAL);

	// left: the list
	auto* left = new wxBoxSizer(wxVERTICAL);
	left->Add(new wxStaticText(this, wxID_ANY, _("Cheats (tick to enable)")), 0, wxBOTTOM, 4);
	m_list = new wxCheckListBox(this, ID_CHEATS_LIST, wxDefaultPosition, wxSize(260, -1));
	left->Add(m_list, 1, wxEXPAND);
	auto* listButtons = new wxBoxSizer(wxHORIZONTAL);
	listButtons->Add(new wxButton(this, ID_CHEATS_ADD, _("Add cheat")), 0, wxRIGHT, 5);
	m_deleteButton = new wxButton(this, ID_CHEATS_DELETE, _("Delete"));
	listButtons->Add(m_deleteButton, 0);
	left->Add(listButtons, 0, wxTOP, 5);
	body->Add(left, 0, wxEXPAND | wxRIGHT, 10);

	// right: the selected cheat
	auto* right = new wxFlexGridSizer(2, 5, 8);
	right->AddGrowableCol(1);
	right->AddGrowableRow(2, 3);
	right->AddGrowableRow(1, 1);

	right->Add(new wxStaticText(this, wxID_ANY, _("Name")), 0, wxALIGN_CENTER_VERTICAL);
	m_name = new wxTextCtrl(this, ID_CHEATS_NAME);
	right->Add(m_name, 1, wxEXPAND);

	right->Add(new wxStaticText(this, wxID_ANY, _("Notes")), 0, wxALIGN_TOP);
	m_notes = new wxTextCtrl(this, ID_CHEATS_NOTES, wxEmptyString, wxDefaultPosition, wxSize(-1, 50), wxTE_MULTILINE);
	right->Add(m_notes, 1, wxEXPAND);

	right->Add(new wxStaticText(this, wxID_ANY, _("Code")), 0, wxALIGN_TOP);
	m_code = new wxTextCtrl(this, ID_CHEATS_CODE, wxEmptyString, wxDefaultPosition, wxSize(-1, 150), wxTE_MULTILINE | wxTE_DONTWRAP);
	m_code->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
	m_code->SetHint("02123450 38A00000");
	right->Add(m_code, 1, wxEXPAND);

	right->AddSpacer(0);
	m_saveButton = new wxButton(this, ID_CHEATS_SAVE, _("Save"));
	right->Add(m_saveButton, 0, wxALIGN_RIGHT);

	body->Add(right, 1, wxEXPAND);
	sizer->Add(body, 1, wxEXPAND | wxALL, 10);

	m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
	sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	auto* bottom = new wxBoxSizer(wxHORIZONTAL);
	bottom->Add(new wxButton(this, ID_CHEATS_OPENFOLDER, _("Open folder")), 0, wxRIGHT, 5);
	bottom->Add(new wxButton(this, ID_CHEATS_RELOAD, _("Reload file")), 0, wxRIGHT, 5);
	bottom->AddStretchSpacer();
	bottom->Add(new wxButton(this, wxID_CLOSE, _("Close")), 0);
	sizer->Add(bottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

	SetSizer(sizer);
	SetMinSize(wxSize(620, 420));

	Bind(wxEVT_LISTBOX, &CheatsWindow::OnSelect, this, ID_CHEATS_LIST);
	Bind(wxEVT_CHECKLISTBOX, &CheatsWindow::OnToggle, this, ID_CHEATS_LIST);
	Bind(wxEVT_TEXT, &CheatsWindow::OnEdited, this, ID_CHEATS_NAME);
	Bind(wxEVT_TEXT, &CheatsWindow::OnEdited, this, ID_CHEATS_NOTES);
	Bind(wxEVT_TEXT, &CheatsWindow::OnEdited, this, ID_CHEATS_CODE);
	Bind(wxEVT_BUTTON, &CheatsWindow::OnAdd, this, ID_CHEATS_ADD);
	Bind(wxEVT_BUTTON, &CheatsWindow::OnDelete, this, ID_CHEATS_DELETE);
	Bind(wxEVT_BUTTON, &CheatsWindow::OnSave, this, ID_CHEATS_SAVE);
	Bind(wxEVT_BUTTON, &CheatsWindow::OnReload, this, ID_CHEATS_RELOAD);
	Bind(wxEVT_BUTTON, &CheatsWindow::OnOpenFolder, this, ID_CHEATS_OPENFOLDER);
	Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); }, wxID_CLOSE);
	Bind(wxEVT_CLOSE_WINDOW, &CheatsWindow::OnClose, this);

	ReloadFromDisk();
}

void CheatsWindow::ReloadFromDisk()
{
	m_cheats = CheatManager::Load(m_titleId);
	RefreshList();
	ShowCheat(m_cheats.empty() ? -1 : 0);
	UpdateStatus();
}

void CheatsWindow::RefreshList()
{
	m_list->Clear();
	for (size_t i = 0; i < m_cheats.size(); i++)
	{
		m_list->Append(wxString::FromUTF8(m_cheats[i].name));
		m_list->Check((unsigned int)i, m_cheats[i].enabled);
	}
}

void CheatsWindow::ShowCheat(int index)
{
	m_selected = index;
	m_loadingEditor = true;
	const bool valid = index >= 0 && index < (int)m_cheats.size();
	if (valid)
	{
		const auto& c = m_cheats[index];
		m_list->SetSelection(index);
		m_name->ChangeValue(wxString::FromUTF8(c.name));
		m_notes->ChangeValue(wxString::FromUTF8(c.notes));
		std::string code;
		for (const auto& l : c.code)
		{
			code += l;
			code += '\n';
		}
		m_code->ChangeValue(wxString::FromUTF8(code));
	}
	else
	{
		m_name->ChangeValue(wxEmptyString);
		m_notes->ChangeValue(wxEmptyString);
		m_code->ChangeValue(wxEmptyString);
	}
	m_name->Enable(valid);
	m_notes->Enable(valid);
	m_code->Enable(valid);
	m_deleteButton->Enable(valid);
	m_loadingEditor = false;
	SetDirty(false);
}

void CheatsWindow::SetDirty(bool dirty)
{
	m_dirty = dirty;
	m_saveButton->Enable(dirty);
}

void CheatsWindow::UpdateStatus()
{
	wxString label = wxString::Format(_("File: %s"), wxHelper::FromPath(CheatManager::GetCheatFile(m_titleId)));
	if (CheatManager::IsRunningTitle(m_titleId))
		label += "\n" + _("The game is running: changes apply immediately.");
	else
		label += "\n" + _("Changes take effect the next time the game starts.");
	m_status->SetLabel(label);
	m_status->Wrap(GetClientSize().GetWidth() - 20);
	Layout();
}

std::vector<std::string> CheatsWindow::EditorCodeLines() const
{
	std::vector<std::string> lines;
	std::istringstream in(ToUtf8(m_code->GetValue()));
	std::string line;
	while (std::getline(in, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(line);
	}
	return lines;
}

bool CheatsWindow::WriteFile()
{
	std::string error;
	if (!CheatManager::Save(m_titleId, m_cheats, error))
	{
		wxMessageBox(wxString::FromUTF8(error), _("Cheats"), wxOK | wxICON_ERROR, this);
		return false;
	}
	UpdateStatus();
	return true;
}

// Copies the editor into the selected cheat and writes the file. Refuses code that doesn't parse,
// so a bad line never reaches the running game.
bool CheatsWindow::SaveEditor()
{
	if (m_selected < 0 || m_selected >= (int)m_cheats.size())
		return true;

	CheatManager::Cheat edited = m_cheats[m_selected];
	edited.name = ToUtf8(m_name->GetValue());
	// names become the [header] line, so keep them to one line without brackets
	for (auto& ch : edited.name)
		if (ch == '\n' || ch == '\r' || ch == '[' || ch == ']')
			ch = ' ';
	if (edited.name.find_first_not_of(' ') == std::string::npos)
	{
		wxMessageBox(_("Please give the cheat a name."), _("Cheats"), wxOK | wxICON_WARNING, this);
		return false;
	}
	edited.notes = ToUtf8(m_notes->GetValue());
	edited.code.clear();
	for (auto& l : EditorCodeLines())
		if (l.find_first_not_of(" \t") != std::string::npos)
			edited.code.push_back(l);

	std::string error;
	if (!CheatManager::Validate(edited, error))
	{
		wxMessageBox(wxString::FromUTF8(error), _("Invalid cheat code"), wxOK | wxICON_WARNING, this);
		return false;
	}

	m_cheats[m_selected] = edited;
	m_list->SetString((unsigned int)m_selected, wxString::FromUTF8(edited.name));
	if (!WriteFile())
		return false;
	ShowCheat(m_selected);
	return true;
}

bool CheatsWindow::ResolveUnsavedChanges()
{
	if (!m_dirty)
		return true;
	const int answer = wxMessageBox(_("Save changes to this cheat?"), _("Cheats"), wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
	if (answer == wxCANCEL)
		return false;
	if (answer == wxYES)
		return SaveEditor();
	SetDirty(false);
	return true;
}

void CheatsWindow::OnSelect(wxCommandEvent& event)
{
	const int index = event.GetSelection();
	if (index == m_selected)
		return;
	if (!ResolveUnsavedChanges())
	{
		m_list->SetSelection(m_selected);
		return;
	}
	ShowCheat(index);
}

void CheatsWindow::OnToggle(wxCommandEvent& event)
{
	const int index = event.GetInt();
	if (index < 0 || index >= (int)m_cheats.size())
		return;
	const bool enable = m_list->IsChecked((unsigned int)index);
	if (enable)
	{
		std::string error;
		if (!CheatManager::Validate(m_cheats[index], error))
		{
			m_list->Check((unsigned int)index, false);
			wxMessageBox(wxString::FromUTF8(error), _("Invalid cheat code"), wxOK | wxICON_WARNING, this);
			return;
		}
	}
	m_cheats[index].enabled = enable;
	if (!WriteFile())
	{
		m_cheats[index].enabled = !enable;
		m_list->Check((unsigned int)index, !enable);
	}
}

void CheatsWindow::OnEdited(wxCommandEvent& event)
{
	if (!m_loadingEditor)
		SetDirty(true);
}

void CheatsWindow::OnAdd(wxCommandEvent& event)
{
	if (!ResolveUnsavedChanges())
		return;
	CheatManager::Cheat cheat;
	cheat.name = ToUtf8(_("New cheat"));
	m_cheats.push_back(cheat);
	if (!WriteFile())
	{
		m_cheats.pop_back();
		return;
	}
	RefreshList();
	ShowCheat((int)m_cheats.size() - 1);
	m_name->SetFocus();
	m_name->SelectAll();
}

void CheatsWindow::OnDelete(wxCommandEvent& event)
{
	if (m_selected < 0 || m_selected >= (int)m_cheats.size())
		return;
	const wxString question = wxString::Format(_("Delete the cheat \"%s\"?"), wxString::FromUTF8(m_cheats[m_selected].name));
	if (wxMessageBox(question, _("Cheats"), wxYES_NO | wxICON_QUESTION, this) != wxYES)
		return;
	const auto removed = m_cheats[m_selected];
	const int at = m_selected;
	m_cheats.erase(m_cheats.begin() + at);
	if (!WriteFile())
	{
		m_cheats.insert(m_cheats.begin() + at, removed);
		return;
	}
	RefreshList();
	ShowCheat(m_cheats.empty() ? -1 : std::min(at, (int)m_cheats.size() - 1));
}

void CheatsWindow::OnSave(wxCommandEvent& event)
{
	SaveEditor();
}

void CheatsWindow::OnReload(wxCommandEvent& event)
{
	if (!ResolveUnsavedChanges())
		return;
	ReloadFromDisk();
	// the file may have been edited by hand; make the running game match it
	CheatManager::Apply(m_titleId, m_cheats);
}

void CheatsWindow::OnOpenFolder(wxCommandEvent& event)
{
	const fs::path folder = CheatManager::GetCheatFolder();
	std::error_code ec;
	fs::create_directories(folder, ec);
	wxLaunchDefaultApplication(wxHelper::FromPath(folder));
}

void CheatsWindow::OnClose(wxCloseEvent& event)
{
	if (event.CanVeto() && !ResolveUnsavedChanges())
	{
		event.Veto();
		return;
	}
	// shown non-modally, so it has to clean itself up
	if (IsModal())
		EndModal(wxID_CLOSE);
	else
		Destroy();
}
