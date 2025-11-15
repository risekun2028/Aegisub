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
#include "include/aegisub/spellchecker.h"
#include "thesaurus.h"
#include "utils.h"

#include <boost/algorithm/string/replace.hpp>
#include <functional>

#include <wx/clipbrd.h>
#include <wx/intl.h>
#include <wx/menu.h>
#include <wx/settings.h>

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
	EDIT_MENU_THES_LANGS
};

SubsTextEditCtrl::SubsTextEditCtrl(wxWindow* parent, wxSize wsize, long style, agi::Context* context)
	: wxStyledTextCtrl(parent, wxID_ANY, wxDefaultPosition, wsize, style)
	, spellchecker(SpellCheckerFactory::GetSpellChecker())
	, thesaurus(agi::make_unique<Thesaurus>())
	, context(context)
{
	SetStyles();

	using std::bind;

	Bind(wxEVT_CHAR_HOOK, &SubsTextEditCtrl::OnKeyDown, this);
	Bind(wxEVT_MOUSEWHEEL, &SubsTextEditCtrl::OnMouseWheel, this);
	Bind(wxEVT_TEXT, [this](wxCommandEvent&) { UpdateStyle(); });  // Apply coloring on text change

	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Cut, this), EDIT_MENU_CUT);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Copy, this), EDIT_MENU_COPY);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::Paste, this), EDIT_MENU_PASTE);
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::SelectAll, this), EDIT_MENU_SELECT_ALL);

	// Bind spell checker and thesaurus menu events
	Bind(wxEVT_MENU, [this](wxCommandEvent &event) {
		std::string suggestion;
		int sugIdx = event.GetId() - EDIT_MENU_THESAURUS_SUGS;
		if (sugIdx >= 0)
			suggestion = thesSugs[sugIdx];
		else
			suggestion = sugs[event.GetId() - EDIT_MENU_SUGGESTIONS];

		long sel_start = currentWordPos.first;
		long sel_end = currentWordPos.first + currentWordPos.second;
		wxString text = GetValue();
		text.replace(sel_start, sel_end - sel_start, to_wx(suggestion));
		SetValue(text);
		SetSelection(sel_start + suggestion.size(), sel_start + suggestion.size());
	}, EDIT_MENU_SUGGESTION, EDIT_MENU_SUGGESTIONS + 1000);

	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetDicLanguage, this, EDIT_MENU_DIC_LANGS, EDIT_MENU_THES_LANGUAGE - 1);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetThesLanguage, this, EDIT_MENU_THES_LANGS, EDIT_MENU_THES_LANGS + LANGS_MAX);

	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->AddWord(currentWord);
		SetFocus();
	}, EDIT_MENU_ADD_TO_DICT);

	Bind(wxEVT_MENU, [this](wxCommandEvent&) {
		if (spellchecker) spellchecker->RemoveWord(currentWord);
		SetFocus();
	}, EDIT_MENU_REMOVE_FROM_DICT);

	if (context) {
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/preserve", context), EDIT_MENU_SPLIT_PRESERVE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/estimate", context), EDIT_MENU_SPLIT_ESTIMATE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/video", context), EDIT_MENU_SPLIT_VIDEO);
		Bind(wxEVT_CONTEXT_MENU, &SubsTextEditCtrl::OnContextMenu, this);
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
		InsertText(GetCurrentPos(), "\\N");
		return;
	}

	// For all other keys, let the STC and OS handle them
	event.Skip();
}

void SubsTextEditCtrl::OnMouseWheel(wxMouseEvent& event) {
	// Handle Ctrl+Wheel for zoom
	if (event.GetModifiers() == wxMOD_CONTROL) {
		if (event.GetWheelRotation() > 0) {
			// Zoom in
			zoom_level++;
		} else if (event.GetWheelRotation() < 0) {
			// Zoom out
			zoom_level--;
		}
		zoom_level = std::max(-10, std::min(10, zoom_level));
		ApplyZoom();
		return;  // We handled it
	}

	// Let the control handle normal scrolling
	event.Skip();
}

void SubsTextEditCtrl::SetStyles() {
	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	font.SetEncoding(wxFONTENCODING_DEFAULT);
	wxString fontname = FontFace("Subtitle/Edit Box");
	if (!fontname.empty()) font.SetFaceName(fontname);
	font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());

	auto default_background = to_wx(OPT_GET("Colour/Subtitle/Background")->GetColor());

	namespace ss = agi::ass::SyntaxStyle;
	
	// Set up each syntax style
	StyleSetFont(ss::NORMAL, font);
	StyleSetForeground(ss::NORMAL, to_wx(OPT_GET("Colour/Subtitle/Syntax/Normal")->GetColor()));
	StyleSetBackground(ss::NORMAL, default_background);

	StyleSetFont(ss::COMMENT, font);
	StyleSetForeground(ss::COMMENT, to_wx(OPT_GET("Colour/Subtitle/Syntax/Comment")->GetColor()));
	StyleSetBackground(ss::COMMENT, default_background);

	StyleSetFont(ss::DRAWING_CMD, font);
	StyleSetForeground(ss::DRAWING_CMD, to_wx(OPT_GET("Colour/Subtitle/Syntax/Drawing Command")->GetColor()));
	StyleSetBackground(ss::DRAWING_CMD, default_background);

	StyleSetFont(ss::DRAWING_X, font);
	StyleSetForeground(ss::DRAWING_X, to_wx(OPT_GET("Colour/Subtitle/Syntax/Drawing X")->GetColor()));

	StyleSetFont(ss::DRAWING_Y, font);
	StyleSetForeground(ss::DRAWING_Y, to_wx(OPT_GET("Colour/Subtitle/Syntax/Drawing Y")->GetColor()));

	StyleSetFont(ss::DRAWING_ENDPOINT_X, font);
	StyleSetForeground(ss::DRAWING_ENDPOINT_X, to_wx(OPT_GET("Colour/Subtitle/Syntax/Drawing X")->GetColor()));
	StyleSetUnderline(ss::DRAWING_ENDPOINT_X, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());

	StyleSetFont(ss::DRAWING_ENDPOINT_Y, font);
	StyleSetForeground(ss::DRAWING_ENDPOINT_Y, to_wx(OPT_GET("Colour/Subtitle/Syntax/Drawing Y")->GetColor()));
	StyleSetUnderline(ss::DRAWING_ENDPOINT_Y, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());

	StyleSetFont(ss::OVERRIDE, font);
	StyleSetForeground(ss::OVERRIDE, to_wx(OPT_GET("Colour/Subtitle/Syntax/Brackets")->GetColor()));
	StyleSetBackground(ss::OVERRIDE, default_background);

	StyleSetFont(ss::PUNCTUATION, font);
	StyleSetForeground(ss::PUNCTUATION, to_wx(OPT_GET("Colour/Subtitle/Syntax/Slashes")->GetColor()));
	StyleSetBackground(ss::PUNCTUATION, default_background);

	StyleSetFont(ss::TAG, font);
	StyleSetForeground(ss::TAG, to_wx(OPT_GET("Colour/Subtitle/Syntax/Tags")->GetColor()));
	StyleSetBackground(ss::TAG, default_background);

	StyleSetFont(ss::ERROR, font);
	StyleSetForeground(ss::ERROR, to_wx(OPT_GET("Colour/Subtitle/Syntax/Error")->GetColor()));
	StyleSetBackground(ss::ERROR, default_background);

	StyleSetFont(ss::PARAMETER, font);
	StyleSetForeground(ss::PARAMETER, to_wx(OPT_GET("Colour/Subtitle/Syntax/Parameters")->GetColor()));
	StyleSetBackground(ss::PARAMETER, default_background);

	StyleSetFont(ss::LINE_BREAK, font);
	StyleSetForeground(ss::LINE_BREAK, to_wx(OPT_GET("Colour/Subtitle/Syntax/Line Break")->GetColor()));
	StyleSetBackground(ss::LINE_BREAK, default_background);

	StyleSetFont(ss::KARAOKE_TEMPLATE, font);
	StyleSetForeground(ss::KARAOKE_TEMPLATE, to_wx(OPT_GET("Colour/Subtitle/Syntax/Karaoke Template")->GetColor()));
	StyleSetBackground(ss::KARAOKE_TEMPLATE, default_background);

	StyleSetFont(ss::KARAOKE_VARIABLE, font);
	StyleSetForeground(ss::KARAOKE_VARIABLE, to_wx(OPT_GET("Colour/Subtitle/Syntax/Karaoke Variable")->GetColor()));
	StyleSetBackground(ss::KARAOKE_VARIABLE, default_background);

	SetCaretForeground(StyleGetForeground(ss::NORMAL));
	StyleSetBackground(wxSTC_STYLE_DEFAULT, default_background);

	// Misspelling indicator
	IndicatorSetStyle(0, wxSTC_INDIC_SQUIGGLE);
	IndicatorSetForeground(0, wxColour(255, 0, 0));

	// IME pending text indicator
	IndicatorSetStyle(1, wxSTC_INDIC_PLAIN);
	IndicatorSetUnder(1, true);

	ApplyZoom();
}

void SubsTextEditCtrl::ApplyZoom() {
	wxFont font = GetFont();
	int base_size = OPT_GET("Subtitle/Edit Box/Font Size")->GetInt();
	int new_size = base_size + zoom_level;
	new_size = std::max(3, std::min(50, new_size));  // Clamp between 3 and 50 points
	font.SetPointSize(new_size);
	SetFont(font);
}

void SubsTextEditCtrl::UpdateStyle() {
	// Apply syntax highlighting with colors to different ASS tags using Scintilla
	AssDialogue *diag = context ? context->selectionController->GetActiveLine() : nullptr;
	bool template_line = diag && diag->Comment && (boost::istarts_with(diag->Effect.get(), "template") || boost::istarts_with(diag->Effect.get(), "mixin"));

	std::string current_text = std::string(GetValue().utf8_str().data());
	if (current_text == line_text) return; // No change, no need to update

	tokenized_line = agi::ass::TokenizeDialogueBody(current_text, template_line);
	agi::ass::SplitWords(current_text, tokenized_line);
	line_text = current_text;

	cursor_pos = -1;
	UpdateCallTip();

#if wxVERSION_NUMBER >= 3100
	StartStyling(0);
#else
	StartStyling(0, 255);
#endif

	if (!OPT_GET("Subtitle/Highlight/Syntax")->GetBool()) {
		SetStyling(line_text.size(), 0);
		return;
	}

	if (line_text.empty()) return;

	SetIndicatorCurrent(0);
	size_t pos = 0;
	for (auto const& style_range : agi::ass::SyntaxHighlight(line_text, tokenized_line, spellchecker.get())) {
		if (style_range.type == agi::ass::SyntaxStyle::SPELLING) {
			SetStyling(style_range.length, agi::ass::SyntaxStyle::NORMAL);
			IndicatorFillRange(pos, style_range.length);
		}
		else {
			SetStyling(style_range.length, style_range.type);
			IndicatorClearRange(pos, style_range.length);
		}
		pos += style_range.length;
	}
}

void SubsTextEditCtrl::UpdateCallTip() {
	if (!OPT_GET("App/Call Tips")->GetBool()) return;
	if (line_text.empty() || cursor_pos < 0 || cursor_pos > (long)line_text.size()) {
		CallTipCancel();
		return;
	}

	// Find the tag at cursor position
	// Implementation for ASS tag call tips (tooltips for tag documentation)
	// This is simplified - full implementation would show tag parameter hints
}

void SubsTextEditCtrl::OnContextMenu(wxContextMenuEvent& event) {
	// KEY FEATURE: Shift+Right-Click shows native OS context menu
	// This gives access to OS-specific features like RTL text display on Ubuntu
	if (wxGetKeyState(WXK_SHIFT)) {
		event.Skip();  // Show native context menu
		return;
	}

	wxPoint pos = event.GetPosition();
	int activePos;
	if (pos == wxDefaultPosition)
		activePos = GetInsertionPoint();
	else
		activePos = PositionFromPoint(ScreenToClient(pos));

	currentWordPos = GetBoundsOfWordAtPosition(activePos);
	currentWord = std::string(GetValue().utf8_str().data()) + "";
	if (currentWordPos.first >= 0 && currentWordPos.first + currentWordPos.second <= (int)currentWord.length()) {
		currentWord = currentWord.substr(currentWordPos.first, currentWordPos.second);
	}

	wxMenu menu;
	
	if (spellchecker) {
		AddSpellCheckerEntries(menu);

		// Append language list
		menu.Append(-1, _("Spell checker language"), GetLanguagesMenu(
			EDIT_MENU_DIC_LANGS,
			to_wx(OPT_GET("Tool/Spell Checker/Language")->GetString()),
			to_wx(spellchecker->GetLanguageList())));
		menu.AppendSeparator();
	}

	AddThesaurusEntries(menu);

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
	SetValue(to_wx(text));
}

std::pair<int, int> SubsTextEditCtrl::GetBoundsOfWordAtPosition(int pos) {
	// Simple word boundary detection for native wxTextCtrl
	// Returns {start_pos, length} of word at position pos
	wxString text = GetValue();
	if (pos < 0 || pos > (int)text.length()) return {0, 0};

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
	if (currentWord.empty()) return;

	if (spellchecker->CanRemoveWord(currentWord))
		menu.Append(EDIT_MENU_REMOVE_FROM_DICT, fmt_tl("Remove \"%s\" from dictionary", currentWord));

	sugs = spellchecker->GetSuggestions(currentWord);
	if (spellchecker->CheckWord(currentWord)) {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No spell checker suggestions"))->Enable(false);
		else {
			auto subMenu = new wxMenu;
			for (size_t i = 0; i < sugs.size(); ++i)
				subMenu->Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

			menu.Append(-1, fmt_tl("Spell checker suggestions for \"%s\"", currentWord), subMenu);
		}
	}
	else {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No correction suggestions"))->Enable(false);

		for (size_t i = 0; i < sugs.size(); ++i)
			menu.Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

		// Append "add word"
		menu.Append(EDIT_MENU_ADD_TO_DICT, fmt_tl("Add \"%s\" to dictionary", currentWord))->Enable(spellchecker->CanAddWord(currentWord));
	}
}

void SubsTextEditCtrl::AddThesaurusEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	auto results = thesaurus->Lookup(currentWord);

	thesSugs.clear();

	if (results.size()) {
		auto thesMenu = new wxMenu;

		int curThesEntry = 0;
		for (auto const& result : results) {
			// Single word, insert directly
			if (result.second.empty()) {
				thesMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(result.first));
				thesSugs.push_back(result.first);
				++curThesEntry;
			}
			// Multiple, create submenu
			else {
				auto subMenu = new wxMenu;
				for (auto const& sug : result.second) {
					subMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(sug));
					thesSugs.push_back(sug);
					++curThesEntry;
				}

				thesMenu->Append(-1, to_wx(result.first), subMenu);
			}
		}

		menu.Append(-1, fmt_tl("Thesaurus suggestions for \"%s\"", currentWord), thesMenu);
	}
	else
		menu.Append(EDIT_MENU_THESAURUS,_("No thesaurus suggestions"))->Enable(false);

	// Append language list
	menu.Append(-1,_("Thesaurus language"), GetLanguagesMenu(
		EDIT_MENU_THES_LANGS,
		to_wx(OPT_GET("Tool/Thesaurus/Language")->GetString()),
		to_wx(thesaurus->GetLanguageList())));
	menu.AppendSeparator();
}

wxMenu *SubsTextEditCtrl::GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs) {
	auto languageMenu = new wxMenu;
	languageMenu->AppendRadioItem(base_id, _("Disable"))->Check(curLang.empty());

	for (size_t i = 0; i < langs.size(); ++i)
		languageMenu->AppendRadioItem(base_id + i + 1, LocalizedLanguageName(langs[i]))->Check(langs[i] == curLang);

	return languageMenu;
}

void SubsTextEditCtrl::OnSetDicLanguage(wxCommandEvent &event) {
	std::vector<std::string> langs = spellchecker->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_DIC_LANGS - 1;
	std::string lang;
	if (index >= 0)
		lang = langs[index];

	OPT_SET("Tool/Spell Checker/Language")->SetString(lang);
}

void SubsTextEditCtrl::OnSetThesLanguage(wxCommandEvent &event) {
	if (!thesaurus) return;

	std::vector<std::string> langs = thesaurus->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_THES_LANGS - 1;
	std::string lang;
	if (index >= 0) lang = langs[index];
	OPT_SET("Tool/Thesaurus/Language")->SetString(lang);
}
