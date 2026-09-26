#pragma once

#include <wx/dialog.h>

#include "Cafe/Cheats/CheatManager.h"

#include <vector>

class wxButton;
class wxCheckListBox;
class wxStaticText;
class wxTextCtrl;

// Per-title cheat list, modelled on Citra's cheats dialog: a checklist of cheats on the left, the
// selected cheat's name, notes and code on the right. Ticking a cheat saves the file straight away
// and, if the game is running, turns the cheat on or off immediately. Code edits need "Save".
class CheatsWindow : public wxDialog
{
public:
	CheatsWindow(wxWindow* parent, uint64 titleId, const wxString& gameName);

private:
	void ReloadFromDisk();
	void RefreshList();
	void ShowCheat(int index);
	void SetDirty(bool dirty);
	void UpdateStatus();
	bool WriteFile();
	bool SaveEditor();
	// Asks what to do with unsaved edits. Returns false if the user cancelled.
	bool ResolveUnsavedChanges();
	std::vector<std::string> EditorCodeLines() const;

	void OnSelect(wxCommandEvent& event);
	void OnToggle(wxCommandEvent& event);
	void OnEdited(wxCommandEvent& event);
	void OnAdd(wxCommandEvent& event);
	void OnDelete(wxCommandEvent& event);
	void OnSave(wxCommandEvent& event);
	void OnReload(wxCommandEvent& event);
	void OnOpenFolder(wxCommandEvent& event);
	void OnClose(wxCloseEvent& event);

	uint64 m_titleId;
	std::vector<CheatManager::Cheat> m_cheats;
	int m_selected = -1;
	bool m_dirty = false;
	bool m_loadingEditor = false;

	wxCheckListBox* m_list{};
	wxTextCtrl* m_name{};
	wxTextCtrl* m_notes{};
	wxTextCtrl* m_code{};
	wxButton* m_saveButton{};
	wxButton* m_deleteButton{};
	wxStaticText* m_status{};
};
