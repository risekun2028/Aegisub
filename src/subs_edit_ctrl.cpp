// Copyright (c) 2021, Qirui Wang
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "subs_edit_ctrl.h"

#include "command/command.h"
#include "compat.h"
#include "options.h"
#include "include/aegisub/context.h"
#include "text_selection_controller.h"
#include "utils.h"

#include <libaegisub/character_count.h>

#include <boost/algorithm/string/replace.hpp>

// Maximum number of languages (locales)
#define LANGS_MAX 1000

/// Event ids
enum {
	EDIT_MENU_SPLIT_PRESERVE = (wxID_HIGHEST + 1) + 4000,
	EDIT_MENU_SPLIT_ESTIMATE,
	EDIT_MENU_SPLIT_VIDEO,
	EDIT_MENU_CUT,
	EDIT_MENU_COPY,
	EDIT_MENU_PASTE,
	EDIT_MENU_SELECT_ALL,
	EDIT_MENU_ADD_TO_DICT,
	EDIT_MENU_REMOVE_FROM_DICT,
	EDIT_MENU_SUGGESTION,
	EDIT_MENU_SUGGESTIONS,
	EDIT_MENU_THESAURUS = (wxID_HIGHEST + 1) + 5000,
	EDIT_MENU_THESAURUS_SUGS,
	EDIT_MENU_DIC_LANGUAGE = (wxID_HIGHEST + 1) + 6000,
	EDIT_MENU_DIC_LANGS,
	EDIT_MENU_THES_LANGUAGE = EDIT_MENU_DIC_LANGUAGE + LANGS_MAX,
	EDIT_MENU_THES_LANGS,
	EDIT_MENU_RTL = (wxID_HIGHEST + 1) + 7000
};

SubsTextEditCtrl::SubsTextEditCtrl(wxWindow* parent, wxSize wsize, long style, agi::Context* context)
	: wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wsize, style | wxTE_MULTILINE)
	, context(context)
{
	SetStyles();

	using std::bind;

	Bind(wxEVT_CHAR_HOOK, &SubsTextEditCtrl::OnKeyDown, this);

	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Cut, this), EDIT_MENU_CUT);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Copy, this), EDIT_MENU_COPY);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Paste, this), EDIT_MENU_PASTE);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::SelectAll, this), EDIT_MENU_SELECT_ALL);

	if (context) {
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/preserve", context), EDIT_MENU_SPLIT_PRESERVE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/estimate", context), EDIT_MENU_SPLIT_ESTIMATE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/video", context), EDIT_MENU_SPLIT_VIDEO);
		Bind(wxEVT_CONTEXT_MENU, &SubsTextEditCtrl::OnContextMenu, this);
		// Bind RTL toggle (fallback) so native mode has an explicit toggle
		Bind(wxEVT_MENU, &SubsTextEditCtrl::OnToggleRTL, this, EDIT_MENU_RTL);
	}

	OPT_SUB("Subtitle/Edit Box/Font Face", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Edit Box/Font Size", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Background", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Normal", &SubsTextEditCtrl::SetStyles, this);
}

SubsTextEditCtrl::~SubsTextEditCtrl() {
}

void SubsTextEditCtrl::OnKeyDown(wxKeyEvent& event) {
	// Handle Shift+Return for soft line breaks
	if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
		long sel_start, sel_end;
		GetSelection(&sel_start, &sel_end);
		wxString data = GetRange(0, sel_start) + to_wx("\\N") + GetRange(sel_end, GetLastPosition());
		SetValue(data);
		SetSelection(sel_start + 2, sel_start + 2);
		return;  // We handled it, don't skip
	}

	// For all other keys, let the native widget and OS handle them
	// This includes Ctrl+Shift+Right for word selection, etc.
	event.Skip();
}

void SubsTextEditCtrl::SetStyles() {
	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	font.SetEncoding(wxFONTENCODING_DEFAULT);
	wxString fontname = FontFace("Subtitle/Edit Box");
	if (!fontname.empty()) font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());
	SetFont(font);

	SetBackgroundColour(to_wx(OPT_GET("Colour/Subtitle/Background")->GetColor()));
	SetForegroundColour(to_wx(OPT_GET("Colour/Subtitle/Syntax/Normal")->GetColor()));
}

void SubsTextEditCtrl::OnContextMenu(wxContextMenuEvent& event) {
	wxMenu menu;

	// Standard actions
	menu.Append(EDIT_MENU_CUT, _("Cu&t"))->Enable(!GetStringSelection().IsEmpty());
	menu.Append(EDIT_MENU_COPY, _("&Copy"))->Enable(!GetStringSelection().IsEmpty());
	menu.Append(EDIT_MENU_PASTE, _("&Paste"))->Enable(CanPaste());
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_SELECT_ALL, _("Select &All"));

	// Split
	if (context) {
		menu.AppendSeparator();
		menu.Append(EDIT_MENU_SPLIT_PRESERVE, _("Split at cursor (preserve times)"));
		menu.Append(EDIT_MENU_SPLIT_ESTIMATE, _("Split at cursor (estimate times)"));
		cmd::Command* split_video = cmd::get("edit/line/split/video");
		menu.Append(EDIT_MENU_SPLIT_VIDEO, split_video->StrMenu(context))->Enable(split_video->Validate(context));
	}

	// Add explicit RTL toggle fallback so native mode always has the option
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_RTL, _("Right to left Reading order"));

	PopupMenu(&menu);
}

void SubsTextEditCtrl::Paste() {
	std::string data = GetClipboard();

	boost::replace_all(data, "\r\n", "\\N");
	boost::replace_all(data, "\n", "\\N");
	boost::replace_all(data, "\r", "\\N");

	long sel_start, sel_end;
	GetSelection(&sel_start, &sel_end);
	wxString data_first_half = GetRange(0, sel_start) + to_wx(data);
	wxString data_full = data_first_half + GetRange(sel_end, GetLastPosition());
	Freeze();
	SetValue(data_first_half);
	sel_start = GetLastPosition();
	SetValue(data_full);
	SetSelection(sel_start, sel_start);
	Thaw();
}

void SubsTextEditCtrl::SetTextTo(std::string const& text) {
	// Mirror STC behaviour: preserve insertion point, update selection controller
	SetEvtHandlerEnabled(false);
	Freeze();

	long insertion_point = GetInsertionPoint();

	// Get current value as std::string
	wxCharBuffer curbuf = GetValue().utf8_str();
	std::string cur = curbuf.data() ? std::string(curbuf.data(), curbuf.length()) : std::string();

	if (static_cast<size_t>(insertion_point) > cur.size())
		; // nothing to do, cur is up-to-date

	// Compute old character index (clamped)
	size_t clamp_pos = std::min<size_t>(cur.size(), static_cast<size_t>(std::max<long>(0, insertion_point)));
	size_t old_pos = agi::CharacterCount(cur.begin(), cur.begin() + clamp_pos, 0);

	if (context) {
		context->textSelectionController->SetSelection(0, 0);
		SetValue(to_wx(text));
		auto pos = agi::IndexOfCharacter(text, old_pos);
		context->textSelectionController->SetSelection(pos, pos);
	}
	else {
		SetSelection(0, 0);
		SetValue(to_wx(text));
		auto pos = agi::IndexOfCharacter(text, old_pos);
		SetSelection(pos, pos);
	}

	SetEvtHandlerEnabled(true);
	Thaw();
}

std::pair<int, int> SubsTextEditCtrl::GetBoundsOfWordAtPosition(int pos) {
	// Simple word boundary detection for native wxTextCtrl
	// Returns {start_pos, length} of word at position pos
	wxString text = GetValue();
	// Handle empty control or invalid position
	if (text.empty() || pos < 0 || pos > (int)text.length()) return {0, 0};

	// Find start of word
	int start = pos;
	while (start > 0 && wxIsalnum(text[start - 1])) {
		start--;
	}

	// Find end of word
	int end = pos;
	while (end < (int)text.length() && wxIsalnum(text[end])) {
		end++;
	}

	return {start, end - start};
}

// Placeholder implementations for menu items (simplified for wxTextCtrl)
void SubsTextEditCtrl::AddSpellCheckerEntries(wxMenu &menu) {
	// Spell checker not implemented for native wxTextCtrl version
}

void SubsTextEditCtrl::AddThesaurusEntries(wxMenu &menu) {
	// Thesaurus not implemented for native wxTextCtrl version
}

wxMenu *SubsTextEditCtrl::GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs) {
	// Placeholder
	return nullptr;
}

void SubsTextEditCtrl::OnToggleRTL(wxCommandEvent &event) {
	wxLayoutDirection cur = GetLayoutDirection();
	wxLayoutDirection next = (cur == wxLayout_RightToLeft) ? wxLayout_LeftToRight : wxLayout_RightToLeft;
	SetLayoutDirection(next);
	// Also update caret/selection behavior by refreshing control
	Refresh();
}
