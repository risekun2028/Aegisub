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
#include "text_selection_controller.h"
#include "selection_controller.h"
#include "thesaurus.h"
#include "utils.h"
#include "format.h"
#include "ass_dialogue.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/character_count.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/spellchecker.h>
#include <libaegisub/calltip_provider.h>

#include <functional>

#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/predicate.hpp>

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
	EDIT_MENU_SPELL_LANGUAGE = (wxID_HIGHEST + 1) + 6000,
	EDIT_MENU_SPELL_LANGS,
	EDIT_MENU_THES_LANGUAGE = EDIT_MENU_SPELL_LANGUAGE + LANGS_MAX,
	EDIT_MENU_THES_LANGS,
	EDIT_MENU_RTL = (wxID_HIGHEST + 1) + 7000
};

SubsTextEditCtrl::SubsTextEditCtrl(wxWindow* parent, wxSize wsize, long style, agi::Context* context)
	: wxStyledTextCtrl(parent, wxID_ANY, wxDefaultPosition, wsize, style)
	, thesaurus(agi::make_unique<Thesaurus>())
	, spellchecker(SpellCheckerFactory::GetSpellChecker())
	, context(context)
{
	// Inject IME helper on macOS and set basic STC properties
#ifdef __WXOSX__
	osx::ime::inject(this);
#endif

	SetWrapMode(wxSTC_WRAP_WORD);
	SetMarginWidth(1, 0);
	UsePopUp(false);

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
	}
	// Bind RTL toggle so native mode has an explicit toggle
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnToggleRTL, this, EDIT_MENU_RTL);

	// Context menu handler (always bound)
	Bind(wxEVT_CONTEXT_MENU, &SubsTextEditCtrl::OnContextMenu, this);

	// When STC text changes, update syntax highlight and emit wxEVT_TEXT for compatibility
	Bind(wxEVT_STC_MODIFIED, [this](wxStyledTextEvent &){
		UpdateSyntaxHighlight();
		wxCommandEvent evt(wxEVT_TEXT);
		evt.SetEventObject(this);
		GetEventHandler()->ProcessEvent(evt);
	});

	Bind(wxEVT_STC_DOUBLECLICK, &SubsTextEditCtrl::OnDoubleClick, this);

	Bind(wxEVT_KILL_FOCUS, &SubsTextEditCtrl::OnLoseFocus, this);

	// If styling is requested, check whether the line text changed and update
	Bind(wxEVT_STC_STYLENEEDED, [this](wxStyledTextEvent&) {
		{
			std::string text = GetTextRaw().data();
			if (text == line_text) return;
			line_text = move(text);
		}

		UpdateSyntaxHighlight();
	});

	Bind(wxEVT_IDLE, std::bind(&SubsTextEditCtrl::UpdateCallTip, this));

	// Bind spell checker suggestion handlers
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::OnUseSuggestion, this, std::placeholders::_1), EDIT_MENU_SUGGESTIONS, EDIT_MENU_SUGGESTIONS+LANGS_MAX);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnAddToDict, this, EDIT_MENU_ADD_TO_DICT);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnRemoveFromDict, this, EDIT_MENU_REMOVE_FROM_DICT);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetSpellLang, this, EDIT_MENU_SPELL_LANGS, EDIT_MENU_SPELL_LANGS+LANGS_MAX);
	// Bind thesaurus suggestion handlers
	Bind(wxEVT_MENU, bind(&SubsTextEditCtrl::OnUseSuggestion, this, std::placeholders::_1), EDIT_MENU_THESAURUS_SUGS, EDIT_MENU_THESAURUS_SUGS+LANGS_MAX);
	Bind(wxEVT_MENU, &SubsTextEditCtrl::OnSetThesLanguage, this, EDIT_MENU_THES_LANGS, EDIT_MENU_THES_LANGS+LANGS_MAX);

	OPT_SUB("Subtitle/Edit Box/Font Face", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Edit Box/Font Size", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Background", &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Normal", &SubsTextEditCtrl::SetStyles, this);

	// Subscribe to the individual syntax color options so style updates when they change
	Subscribe("Normal");
	Subscribe("Comment");
	Subscribe("Drawing Command");
	Subscribe("Drawing X");
	Subscribe("Drawing Y");
	OPT_SUB("Colour/Subtitle/Syntax/Underline/Drawing Endpoint", &SubsTextEditCtrl::SetStyles, this);
	Subscribe("Brackets");
	Subscribe("Slashes");
	Subscribe("Tags");
	Subscribe("Error");
	Subscribe("Parameters");
	Subscribe("Line Break");
	Subscribe("Karaoke Template");
	Subscribe("Karaoke Variable");

	OPT_SUB("Subtitle/Highlight/Syntax", &SubsTextEditCtrl::UpdateSyntaxHighlight, this);
	OPT_SUB("App/Call Tips", &SubsTextEditCtrl::UpdateCallTip, this);
}

SubsTextEditCtrl::~SubsTextEditCtrl() {
}

void SubsTextEditCtrl::OnKeyDown(wxKeyEvent& event) {
	// Handle Shift+Return for soft line breaks
	if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
		int sel_start = GetSelectionStart();
		int sel_end = GetSelectionEnd();
		wxCharBuffer old = GetTextRaw();
		std::string data(old.data(), sel_start);
		data.append(OPT_GET("Subtitle/Edit Box/Soft Line Break")->GetBool() ? "\\n" : "\\N");
		data.append(old.data() + sel_end, old.length() - sel_end);
		SetTextRaw(data.c_str());

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

	auto default_background = to_wx(OPT_GET("Colour/Subtitle/Background")->GetColor());

	// Apply to STC styles using helper to match the original STC implementation
	namespace ss = agi::ass::SyntaxStyle;
	SetSyntaxStyle(ss::NORMAL, font, "Normal", default_background);
	SetSyntaxStyle(ss::COMMENT, font, "Comment", default_background);
	SetSyntaxStyle(ss::DRAWING_CMD, font, "Drawing Command", default_background);
	SetSyntaxStyle(ss::DRAWING_X, font, "Drawing X", default_background);
	SetSyntaxStyle(ss::DRAWING_Y, font, "Drawing Y", default_background);
	SetSyntaxStyle(ss::DRAWING_ENDPOINT_X, font, "Drawing X", default_background);
	SetSyntaxStyle(ss::DRAWING_ENDPOINT_Y, font, "Drawing Y", default_background);
	StyleSetUnderline(ss::DRAWING_ENDPOINT_X, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());
	StyleSetUnderline(ss::DRAWING_ENDPOINT_Y, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());
	SetSyntaxStyle(ss::OVERRIDE, font, "Brackets", default_background);
	SetSyntaxStyle(ss::PUNCTUATION, font, "Slashes", default_background);
	SetSyntaxStyle(ss::TAG, font, "Tags", default_background);
	SetSyntaxStyle(ss::ERROR, font, "Error", default_background);
	SetSyntaxStyle(ss::PARAMETER, font, "Parameters", default_background);
	SetSyntaxStyle(ss::LINE_BREAK, font, "Line Break", default_background);
	SetSyntaxStyle(ss::KARAOKE_TEMPLATE, font, "Karaoke Template", default_background);
	SetSyntaxStyle(ss::KARAOKE_VARIABLE, font, "Karaoke Variable", default_background);

	StyleSetBackground(wxSTC_STYLE_DEFAULT, default_background);

	SetCaretForeground(StyleGetForeground(ss::NORMAL));

	// Misspelling indicator
	IndicatorSetStyle(0, wxSTC_INDIC_SQUIGGLE);
	IndicatorSetForeground(0, wxColour(255,0,0));

	// IME pending text indicator
	IndicatorSetStyle(1, wxSTC_INDIC_PLAIN);
	IndicatorSetUnder(1, true);
}

void SubsTextEditCtrl::OnContextMenu(wxContextMenuEvent& event) {
	wxPoint pos = event.GetPosition();
	int activePos;
	if (pos == wxDefaultPosition)
		activePos = GetCurrentPos();
	else
		activePos = PositionFromPoint(ScreenToClient(pos));

	// Ensure we have the raw UTF-8 text available in line_text
	wxCharBuffer raw = GetTextRaw();
	line_text = raw.data() ? raw.data() : std::string();

	currentWordPos = GetBoundsOfWordAtPosition(activePos);
	if (currentWordPos.second > 0 && (size_t)(currentWordPos.first + currentWordPos.second) <= line_text.size()) {
		currentWord = line_text.substr(currentWordPos.first, currentWordPos.second);
	} else {
		currentWord.clear();
	}

	wxMenu menu;

	// Standard actions
	menu.Append(EDIT_MENU_CUT, _("Cu&t"))->Enable(!GetSelectedText().IsEmpty());
	menu.Append(EDIT_MENU_COPY, _("&Copy"))->Enable(!GetSelectedText().IsEmpty());
	menu.Append(EDIT_MENU_PASTE, _("&Paste"))->Enable(CanPaste());
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_SELECT_ALL, _("Select &All"));

	// Spell checker
	if (spellchecker)
		AddSpellCheckerEntries(menu);

	// Thesaurus
	AddThesaurusEntries(menu);

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

	// Use GetPopupMenuSelectionFromUser to get the menu choice directly,
	// then handle RTL toggle explicitly (more reliable than event routing in PopupMenu)
	int menuResult = GetPopupMenuSelectionFromUser(menu);
	if (menuResult == EDIT_MENU_RTL) {
		wxLayoutDirection cur = GetLayoutDirection();
		wxLayoutDirection next = (cur == wxLayout_RightToLeft) ? wxLayout_LeftToRight : wxLayout_RightToLeft;
		SetLayoutDirection(next);
		Refresh();
	}
}

void SubsTextEditCtrl::OnLoseFocus(wxFocusEvent &event) {
	CallTipCancel();
	event.Skip();
}

void SubsTextEditCtrl::OnDoubleClick(wxStyledTextEvent &evt) {
	int pos = evt.GetPosition();
	if (pos == -1 && !tokenized_line.empty()) {
		auto tok = tokenized_line.back();
		SetSelection(line_text.size() - tok.length, line_text.size());
	}
	else {
		auto bounds = GetBoundsOfWordAtPosition(evt.GetPosition());
		if (bounds.second != 0)
			SetSelection(bounds.first, bounds.first + bounds.second);
		else
			evt.Skip();
	}
}

void SubsTextEditCtrl::UpdateCallTip() {
	if (!OPT_GET("App/Call Tips")->GetBool()) return;

	int pos = GetCurrentPos();
	if (pos == cursor_pos) return;
	cursor_pos = pos;

	agi::Calltip new_calltip = agi::GetCalltip(tokenized_line, line_text, pos);

	if (!new_calltip.text) {
		CallTipCancel();
		return;
	}

	if (!CallTipActive() || calltip_position != new_calltip.tag_position || calltip_text != new_calltip.text)
		CallTipShow(new_calltip.tag_position, wxString::FromUTF8Unchecked(new_calltip.text));

	calltip_position = new_calltip.tag_position;
	calltip_text = new_calltip.text;

	CallTipSetHighlight(new_calltip.highlight_start, new_calltip.highlight_end);
}

void SubsTextEditCtrl::SetSyntaxStyle(int id, wxFont &font, std::string const& name, wxColor const& default_background) {
	StyleSetFont(id, font);
	StyleSetBold(id, OPT_GET("Colour/Subtitle/Syntax/Bold/" + name)->GetBool());
	StyleSetForeground(id, to_wx(OPT_GET("Colour/Subtitle/Syntax/" + name)->GetColor()));
	const agi::OptionValue *background = OPT_GET("Colour/Subtitle/Syntax/Background/" + name);
	if (background && background->GetType() == agi::OptionType::Color)
		StyleSetBackground(id, to_wx(background->GetColor()));
	else
		StyleSetBackground(id, default_background);
}

void SubsTextEditCtrl::Paste() {
	std::string data = GetClipboard();

	boost::replace_all(data, "\r\n", "\\N");
	boost::replace_all(data, "\n", "\\N");
	boost::replace_all(data, "\r", "\\N");

	wxCharBuffer old = GetTextRaw();
	std::string cur = old.data() ? old.data() : std::string();

	data.insert(0, cur.data(), GetSelectionStart());
	int sel_start = (int)data.size();
	data.append(cur.data() + GetSelectionEnd());

	SetTextRaw(data.c_str());

	SetSelectionStart(sel_start);
	SetSelectionEnd(sel_start);
}

void SubsTextEditCtrl::SetTextTo(std::string const& text) {
	// Mirror STC behaviour: preserve insertion point, update selection controller
	SetEvtHandlerEnabled(false);
	Freeze();

	long insertion_point = GetInsertionPoint();

	// Get current value as std::string (raw UTF-8)
	wxCharBuffer curbuf = GetTextRaw();
	std::string cur = curbuf.data() ? std::string(curbuf.data(), curbuf.length()) : std::string();

	if (static_cast<size_t>(insertion_point) > cur.size())
		; // nothing to do, cur is up-to-date

	// Compute old character index (clamped)
	size_t clamp_pos = std::min<size_t>(cur.size(), static_cast<size_t>(std::max<long>(0, insertion_point)));
	size_t old_pos = agi::CharacterCount(cur.begin(), cur.begin() + clamp_pos, 0);

	if (context) {
		context->textSelectionController->SetSelection(0, 0);
		SetTextRaw(text.c_str());
		auto pos = agi::IndexOfCharacter(text, old_pos);
		context->textSelectionController->SetSelection(pos, pos);
	}
	else {
		SetSelection(0, 0);
		SetTextRaw(text.c_str());
		auto pos = agi::IndexOfCharacter(text, old_pos);
		SetSelection(pos, pos);
	}

	SetEvtHandlerEnabled(true);
	Thaw();
}

std::pair<int, int> SubsTextEditCtrl::GetBoundsOfWordAtPosition(int pos) {
	int len = 0;
	for (auto const& tok : tokenized_line) {
		if (len + (int)tok.length > pos) {
			if (tok.type == agi::ass::DialogueTokenType::WORD)
				return {len, tok.length};
			return {0, 0};
		}
		len += tok.length;
	}

	return {0, 0};
}

// Placeholder implementations for menu items (simplified for wxTextCtrl)
void SubsTextEditCtrl::AddSpellCheckerEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	if (spellchecker->CanRemoveWord(currentWord))
		menu.Append(EDIT_MENU_REMOVE_FROM_DICT, fmt_tl("Remove \"%s\" from dictionary", currentWord));

	sugs = spellchecker->GetSuggestions(currentWord);
	if (spellchecker->CheckWord(currentWord)) {
		// Word is spelled correctly
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
		// Word is misspelled - show suggestions directly
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No correction suggestions"))->Enable(false);
		else {
			for (size_t i = 0; i < sugs.size(); ++i)
				menu.Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));
		}

		// Append "add word" option for misspelled words
		menu.Append(EDIT_MENU_ADD_TO_DICT, fmt_tl("Add \"%s\" to dictionary", currentWord))->Enable(spellchecker->CanAddWord(currentWord));
	}
	menu.Append(-1,_("Spell checker language"), GetLanguagesMenu(
		EDIT_MENU_SPELL_LANGS,
		to_wx(OPT_GET("Tool/Spell Checker/Language")->GetString()),
		to_wx(spellchecker->GetLanguageList())));
	menu.AppendSeparator();
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

void SubsTextEditCtrl::OnUseSuggestion(wxCommandEvent &event) {
	std::string suggestion;
	int eventId = event.GetId();

	// Check if this is a spell checker suggestion
	if (eventId >= EDIT_MENU_SUGGESTIONS && eventId < EDIT_MENU_SUGGESTIONS + LANGS_MAX) {
		int sugIdx = eventId - EDIT_MENU_SUGGESTIONS;
		if (sugIdx >= 0 && (size_t)sugIdx < sugs.size())
			suggestion = sugs[sugIdx];
		else
			return;
	}
	// Or a thesaurus suggestion
	else if (eventId >= EDIT_MENU_THESAURUS_SUGS && eventId < EDIT_MENU_THESAURUS_SUGS + LANGS_MAX) {
		int sugIdx = eventId - EDIT_MENU_THESAURUS_SUGS;
		if (sugIdx >= 0 && (size_t)sugIdx < thesSugs.size())
			suggestion = thesSugs[sugIdx];
		else
			return;
	}
	else
		return;

	size_t pos;
	while ((pos = suggestion.rfind('(')) != std::string::npos) {
		// If there's only one suggestion for a word it'll be in the form "(noun) word",
		// so we need to trim the "(noun) " part
		if (pos == 0) {
			pos = suggestion.find(')');
			if (pos != std::string::npos) {
				if (pos + 1< suggestion.size() && suggestion[pos + 1] == ' ') ++pos;
				suggestion.erase(0, pos + 1);
			}
			break;
		}

		// Some replacements have notes about their usage after the word in the
		// form "word (generic term)" that we need to remove (plus the leading space)
		suggestion.resize(pos - 1);
	}

	// line_text needs to get cleared before SetTextRaw to ensure it gets reparsed
	std::string new_text;
	swap(line_text, new_text);
	SetTextRaw(new_text.replace(currentWordPos.first, currentWordPos.second, suggestion).c_str());

	SetSelection(currentWordPos.first, currentWordPos.first + (int)suggestion.size());
	SetFocus();
}

void SubsTextEditCtrl::OnSetThesLanguage(wxCommandEvent &event) {
	if (!thesaurus) return;

	std::vector<std::string> langs = thesaurus->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_THES_LANGS - 1;
	std::string lang;
	if (index >= 0 && (size_t)index < langs.size())
		lang = langs[index];

	OPT_SET("Tool/Thesaurus/Language")->SetString(lang);
}

void SubsTextEditCtrl::OnSetSpellLang(wxCommandEvent &event) {
	if (!spellchecker) return;

	std::vector<std::string> langs = spellchecker->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_SPELL_LANGS - 1;
	std::string lang;
	if (index >= 0 && (size_t)index < langs.size())
		lang = langs[index];

	OPT_SET("Tool/Spell Checker/Language")->SetString(lang);
}

void SubsTextEditCtrl::OnAddToDict(wxCommandEvent &event) {
	if (spellchecker)
		spellchecker->AddWord(currentWord);
}

void SubsTextEditCtrl::OnRemoveFromDict(wxCommandEvent &event) {
	if (spellchecker)
		spellchecker->RemoveWord(currentWord);
}

void SubsTextEditCtrl::Subscribe(std::string const& name) {
	OPT_SUB("Colour/Subtitle/Syntax/" + name, &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Background/" + name, &SubsTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Bold/" + name, &SubsTextEditCtrl::SetStyles, this);
}

void SubsTextEditCtrl::UpdateSyntaxHighlight() {
	if (!OPT_GET("Subtitle/Highlight/Syntax")->GetBool()) {
		return;
	}

	wxCharBuffer raw = GetTextRaw();
	line_text = raw.data() ? raw.data() : std::string();

	// Tokenize the line for syntax analysis
	AssDialogue *diag = context ? context->selectionController->GetActiveLine() : nullptr;
	bool template_line = diag && diag->Comment && (boost::istarts_with(diag->Effect.get(), "template") || boost::istarts_with(diag->Effect.get(), "mixin"));

	tokenized_line = agi::ass::TokenizeDialogueBody(line_text, template_line);
	agi::ass::SplitWords(line_text, tokenized_line);

#if wxVERSION_NUMBER >= 3100
	StartStyling(0);
#else
	StartStyling(0, 255);
#endif

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


void SubsTextEditCtrl::OnToggleRTL(wxCommandEvent &event) {
	wxLayoutDirection cur = GetLayoutDirection();
	wxLayoutDirection next = (cur == wxLayout_RightToLeft) ? wxLayout_LeftToRight : wxLayout_RightToLeft;
	SetLayoutDirection(next);
	// Also update caret/selection behavior by refreshing control
	Refresh();
}
