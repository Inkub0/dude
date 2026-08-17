#ifndef IMGUI_DISABLE

#include <algorithm> // std::sort - TODO: replace with something custom..

#include "sys/sys_sdl.h"

#define IMGUI_DEFINE_MATH_OPERATORS

#include "Common.h"

#include "idlib/LangDict.h"

#include "KeyInput.h"
#include "UsercmdGen.h" // key bindings
#include "DeclEntityDef.h"
#include "Session_local.h" // sessLocal.GetActiveMenu()

#include "sys/sys_imgui.h"
#include "../libs/imgui/imgui_internal.h"

#include "renderer/tr_local.h" // render cvars
#include "renderer/Material.h" // idMaterial + pbrCategory_t (PBR material editor)
#include "sound/snd_local.h" // sound cvars

extern const char* D3_GetGamepadStartButtonName();

extern idCVar imgui_style;

extern idCVar r_customWidth;
extern idCVar r_customHeight;
extern idCVar com_maxFPS;

extern bool R_GetModeInfo( int *width, int *height, int mode );

namespace {

const char* GetLocalizedString( const char* id, const char* fallback )
{
	if ( id == nullptr || id[0] == '\0' ) {
		return fallback;
	}
	const char* ret = common->GetLanguageDict()->GetString( id );
	if ( ret == nullptr || ret[0] == '\0'
	    || ( ret[0] == '#' && idStr::Cmpn( ret, STRTABLE_ID, STRTABLE_ID_LENGTH ) == 0 ) ) {
		ret = fallback;
	}
	return ret;
}

// TODO: move the following two functions into sys_imgui.cpp ?
static void AddTooltip( const char* text )
{
	if ( ImGui::BeginItemTooltip() )
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::TextUnformatted( text );
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

static void AddDescrTooltip( const char* description )
{
	if ( description != nullptr ) {
		ImGui::SameLine();
		ImGui::TextDisabled( "(?)" );
		AddTooltip( description );
	}
}



/*************************
 *                       *
 *    Keybinding Menu    *
 *                       *
 *************************/

struct BindingEntry;
static BindingEntry* FindBindingEntryForKey( int keyNum );
static idCVar imgui_numBindingColumns( "imgui_numBindingColumns", "3", CVAR_ARCHIVE|CVAR_SYSTEM|CVAR_INTEGER, "Number of columns with bindings in DUDE settings menu's Bindings tab", 1, 10 );

static int rebindKeyNum = -1; // only used for HandleRebindPopup()
static BindingEntry* rebindOtherEntry = nullptr; // ditto

static bool IsKeyPressed( ImGuiKey key ) {
	return ImGui::IsKeyPressed( key, false );
}

// is Enter (or Keypad Enter) or gamepad A (or equivalent on non-xinput-devices) pressed?
// used for confirmation in popup dialogs, and to initiate key binding in the binding tables
static bool IsConfirmKeyPressed() {
	return IsKeyPressed( ImGuiKey_Enter ) || IsKeyPressed( ImGuiKey_KeypadEnter )
	       || IsKeyPressed( ImGuiKey_GamepadFaceDown );
}

// is Delete, Backspace or gamepad Y (or equivalent on non-xinput-devices) pressed?
static bool IsClearKeyPressed() {
	return IsKeyPressed( ImGuiKey_Delete ) || IsKeyPressed( ImGuiKey_Backspace )
	       || IsKeyPressed( ImGuiKey_GamepadFaceUp );
}

// is Escape or gamepad Start or gamepad B (or equivalents on non-xinput-devices) pressed?
static bool IsCancelKeyPressed() {
	// using Escape, gamepad Start and gamepad B for cancel, except in the
	// binding case, there only Esc and Start work (so gamepad B can be bound),
	// but the binding popup doesn't use this function anyway

	// Note: In Doom3, Escape opens/closes the main menu, so in DUDE the gamepad Start button
	//       behaves the same, incl. the specialty that it can't be bound by the user
	return IsKeyPressed( ImGuiKey_Escape ) || IsKeyPressed( ImGuiKey_GamepadFaceRight )
	       || IsKeyPressed( ImGuiKey_GamepadStart );
}

static const char* GetGamepadStartName() {
	return D3_GetGamepadStartButtonName();
}

static const char* GetGamepadCancelButtonNames() {
	static char ret[64];
	// on xbox: "Pad B or Pad Start"
	D3_snprintfC99( ret, sizeof(ret), "%s or %s", Sys_GetLocalizedJoyKeyName( K_JOY_BTN_EAST ), GetGamepadStartName() );
	return ret;
}

static const char* GetGamepadBindNowButtonName() { // TODO: rename to confirm or sth
	return Sys_GetLocalizedJoyKeyName( K_JOY_BTN_SOUTH ); // xbox A
}

static const char* GetGamepadUnbindButtonName() {
	return Sys_GetLocalizedJoyKeyName( K_JOY_BTN_NORTH ); // xbox Y
}


const char* GetKeyName( int keyNum, bool localized = true )
{
	if( keyNum <= 0 )
		return "<none>";

	if ( keyNum >= 'a' && keyNum <= 'z' ) {
		static char oneChar[2] = {};
		oneChar[0] = keyNum - 32; // to uppercase
		return oneChar;
	}
	// handle scancodes separately, because ImGui uses UTF-8, while DUDE uses ISO8859-1
	if ( keyNum >= K_FIRST_SCANCODE && keyNum <= K_LAST_SCANCODE ) {
		const char* scName = NULL;
		if ( localized ) {
			scName = Sys_GetLocalizedScancodeNameUTF8( keyNum );
		} else {
			scName = Sys_GetScancodeName( keyNum );
		}
		if ( scName != NULL ) {
			return scName;
		}
	}

	return idKeyInput::KeyNumToString( keyNum, localized );
}

// background color for the first column of the binding table, that contains the name of the command
static ImU32 displayNameBGColor = 0;

static const ImVec4 RedButtonColor(1.00f, 0.17f, 0.17f, 0.58f);
static const ImVec4 RedButtonHoveredColor(1.00f, 0.17f, 0.17f, 1.00f);
static const ImVec4 RedButtonActiveColor(1.00f, 0.37f, 0.37f, 1.00f);

static float CalcDialogButtonWidth()
{
	// with the standard font, 120px wide Ok/Cancel buttons look good,
	// this text (+default padding) has that width there
	float testTextWidth = ImGui::CalcTextSize( "Ok or Cancel ???" ).x;
	float framePadWidth = ImGui::GetStyle().FramePadding.x;
	return testTextWidth + 2.0f * framePadWidth;
}


enum BindingEntrySelectionState {
	BESS_NotSelected = 0,
	BESS_Selected,
	BESS_WantBind,
	BESS_WantClear,
	BESS_WantRebind // we were in WantBind, but the key is already bound to another command, so show a confirmation popup
};

struct BoundKey {
	int keyNum = -1;
	idStr keyName;
	idStr internalKeyName; // the one used in bind commands in the D3 console and config

	void Set( int _keyNum )
	{
		keyNum = _keyNum;
		keyName = GetKeyName( _keyNum, true );
		internalKeyName = GetKeyName( _keyNum, false );
	}

	void Clear()
	{
		keyNum = -1;
		keyName = "";
		internalKeyName = "";
	}

	BoundKey() = default;

	BoundKey ( int _keyNum ) {
		Set( _keyNum );
	}

};

struct BindingEntryTemplate {
	const char* command;
	const char* name;
	const char* nameLocStr;
	const char* description;
};

struct BindingEntry {
	idStr command; // "_impulse3" or "_forward" or similar - or "" for heading entry
	idStr displayName;
	idList<BoundKey> bindings;

	const char* description = nullptr;

	enum {
		BIND_NONE   = -1, // no binding currently selected
		// all are selected (clicked command name column): clear all,
		//   or add a new binding in some visible column (idx in bindings < numBindingColumns)
		BIND_ALL    = -2,
		// append new binding at the end, or set it in unused bindings entry, if any (used by AllBindingsWindow)
		BIND_APPEND = -3
	};
	// which binding is currently selected in the UI, if any (and only if this
	//   binding entry is currently active according to Draw()'s oldSelState)
	int selectedBinding = BIND_NONE; // index in bindings or one of the enum values

	BindingEntry() = default;

	BindingEntry( const char* _displayName ) : displayName(_displayName) {}

	BindingEntry( const char* _command, const char* _displayName, const char* descr = nullptr )
	: command( _command ), displayName( _displayName ), description( descr ) {}

	BindingEntry( const idStr& _command, const idStr& _displayName, const char* descr = nullptr )
		: command( _command ), displayName( _displayName ), description( descr ) {}

	BindingEntry( const idStr& _command, const char* _displayName, const char* descr = nullptr )
		: command( _command ), displayName( _displayName ), description( descr ) {}

	BindingEntry( const BindingEntryTemplate& bet )
	: command( bet.command ), description( bet.description ) {
		displayName = GetLocalizedString( bet.nameLocStr, bet.name );
		displayName.StripTrailingWhitespace();
	}

	bool IsHeading() const
	{
		return command.Length() == 0;
	}

	// only removes the entry from bindings, does *not* unbind!
	void RemoveBindingEntry( unsigned idx )
	{
		if ( idx < (unsigned)bindings.Num() ) {
			bindings.RemoveIndex( idx );
		}
	}

	// remove all entries from bindings that don't have a key set
	void CompactBindings()
	{
		for ( int i = bindings.Num() - 1; i >= 0; --i ) {
			if ( bindings[i].keyNum == -1 ) {
				RemoveBindingEntry( i );
			}
		}
	}

	// also updates this->selectedColumn
	void UpdateSelectionState( int bindIdx, /* in+out */ BindingEntrySelectionState& selState )
	{
		// if currently a popup is shown for creating a new binding or clearing one (BESS_WantBind
		// or BESS_WantClear), everything is still rendered, but in a disabled (greyed out) state
		// and shouldn't handle any input => then there's not much to do here,
		//  except for highlighting at the end of the function
		if ( selState < BESS_WantBind ) {
			if ( ImGui::IsItemFocused() ) {
				// Note: even when using the mouse, clicking a selectable will make it focused,
				//  so it's possible to select a command (or specific binding of a command)
				//  with the mouse and then press Enter to (re)bind it or Delete to clear it.
				//  So whether something is selected mostly is equivalent to it being focused.
				//  In the initial development of this code that wasn't the case, so there
				//  *might* be some small inconsistencies due to that; but also intentional
				//  special cases, like a binding entry being drawn as selected while
				//  one of the popups is open to modify it
				//  (=> it doesn't have focus then because the popup has focus)
				//  That's not just cosmetical, selState and selectedBinding are
				//  used to configure the popup.

				selectedBinding = bindIdx;

				if ( IsConfirmKeyPressed() ) {
					selState = BESS_WantBind;
				} else if ( IsClearKeyPressed() ) {
					bool nothingToClear = false;
					if ( bindIdx == BIND_ALL ) {
						if ( bindings.Num() == 0 ) {
							D3::ImGuiHooks::ShowWarningOverlay( "No keys are bound to this command, so there's nothing to unbind" );
							nothingToClear = true;
						}
					} else if ( bindIdx < 0 || bindIdx >= bindings.Num() || bindings[bindIdx].keyNum == -1 ) {
						D3::ImGuiHooks::ShowWarningOverlay( "No bound key selected for unbind" );
						nothingToClear = true;
					}

					selState = nothingToClear ? BESS_Selected : BESS_WantClear;
				} else if ( selState == BESS_NotSelected ) {
					selState = BESS_Selected;
				}
			} else if (selectedBinding == bindIdx && selState != BESS_NotSelected) {
				// apparently this was still selected last frame, but is not focused anymore => unselect it
				selState = BESS_NotSelected;
			}

			if ( ImGui::IsItemHovered() ) { // mouse cursor is on this item
				if ( bindIdx == BIND_ALL ) {
					// if the first column (command name, like "Move Left") is hovered, highlight the whole row
					// A normal Selectable would use ImGuiCol_HeaderHovered, but I use that as the "selected"
					// color (in Draw()), so use the next brighter thing (ImGuiCol_HeaderActive) here.
					ImU32 highlightRowColor = ImGui::GetColorU32( ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive) );
					ImGui::TableSetBgColor( ImGuiTableBgTarget_RowBg0, highlightRowColor );
				}

				if ( ImGui::IsMouseDoubleClicked( 0 ) ) {
					selState = BESS_WantBind;
					selectedBinding = bindIdx;
				}
				// Note: single-clicking an item gives it focus, so that's implictly
				//  handled above in `if ( ImGui::IsItemFocused() ) { ...`
			}
		}

		// this column is selected => highlight it
		if ( selState != BESS_NotSelected && selectedBinding == bindIdx ) {
			// ImGuiCol_Header would be the regular "selected cell/row" color that Selectable would use
			// but ImGuiCol_HeaderHovered is more visible, IMO
			ImU32 highlightRowColor = ImGui::GetColorU32( ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered) );
			ImGui::TableSetBgColor( ImGuiTableBgTarget_CellBg, highlightRowColor );

			if ( bindIdx == BIND_ALL ) {
				// the displayName column is selected => highlight the whole row
				ImGui::TableSetBgColor( ImGuiTableBgTarget_RowBg0, highlightRowColor );
				// (yes, still set the highlight color for ImGuiTableBgTarget_CellBg above for extra
				//  highlighting of the column 0 cell, otherwise it'd look darker due to displayNameBGColor)
			}
		}
	}

	bool DrawAllBindingsWindow( /* in+out */ BindingEntrySelectionState& selState, bool newOpen, const ImVec2& btnMin, const ImVec2& btnMax )
	{
		bool showThisMenu = true;
		idStr menuWinTitle = idStr::Format( "All keys bound to %s###allBindingsWindow", displayName.c_str() );
		int numBindings = bindings.Num();

		ImGuiWindowFlags menuWinFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
		const float fontSize = ImGui::GetFontSize();
		ImVec2 winMinSize = ImGui::CalcTextSize( menuWinTitle, nullptr, true );
		winMinSize.x += fontSize * 2.0f;
		const ImGuiViewport& viewPort = *ImGui::GetMainViewport();
		ImVec2 maxWinSize( viewPort.WorkSize.x, viewPort.WorkSize.y * 0.9f );
		// make sure the window is big enough to show the full title (incl. displayName)
		// and that it fits into the screen (it can scroll if it gets too long)
		ImGui::SetNextWindowSizeConstraints( winMinSize, maxWinSize );

		static ImVec2 winPos;
		if ( newOpen ) {
			// position the window right next to the [++] button that opens/closes it
			winPos = btnMin;
			winPos.x = btnMax.x + ImGui::GetStyle().ItemInnerSpacing.x;
			ImGui::OpenPopup( menuWinTitle );
			ImGui::SetNextWindowPos(winPos);
			ImGui::SetNextWindowFocus();
		}

		if ( ImGui::Begin( menuWinTitle, &showThisMenu, menuWinFlags ) )
		{
			ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg;
			if ( numBindings > 0 && ImGui::BeginTable( "AllBindingsForCommand", 2, tableFlags ) ) {
				ImGui::TableSetupColumn("command", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex( 0 );

				// turn the next button (Unbind all) red
				ImGui::PushStyleColor( ImGuiCol_Button, RedButtonColor );
				ImGui::PushStyleColor( ImGuiCol_ButtonHovered, RedButtonHoveredColor );
				ImGui::PushStyleColor( ImGuiCol_ButtonActive,  RedButtonActiveColor );

				ImGui::Indent();
				if ( ImGui::Button( idStr::Format( "Unbind all" ) ) ) {
					selState = BESS_WantClear;
					selectedBinding = BIND_ALL;
				} else {
					ImGui::SetItemTooltip( "Remove all keybindings for %s", displayName.c_str() );
				}

				ImGui::Unindent();
				ImGui::PopStyleColor(3); // return to normal button color

				ImGui::TableSetColumnIndex( 1 );

				float helpHoverWidth = ImGui::CalcTextSize("(?)").x;
				float offset = ImGui::GetContentRegionAvail().x - helpHoverWidth;
				ImGui::SetCursorPosX( ImGui::GetCursorPosX() + offset );
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled( "(?)" );
				if ( ImGui::BeginItemTooltip() ) {
					ImGui::PushTextWrapPos( ImGui::GetFontSize() * 35.0f );
					ImGui::Text( "You can close this window with Escape (or %s) or by clicking the little (x) button or by clicking the [++] button again.",
								 GetGamepadCancelButtonNames() );
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}

				ImGui::Spacing();

				ImU32 highlightRowColor = 0;
				if ( selectedBinding == BIND_ALL ) {
					highlightRowColor = ImGui::GetColorU32( ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered) );
				}

				ImGui::Indent( fontSize * 0.5f );

				for ( int bnd = 0; bnd < numBindings; ++bnd ) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex( 0 );

					ImGui::PushID( bnd ); // the buttons have the same names in every row, so push the row number as ID

					bool colHasBinding = bindings[bnd].keyNum != -1;
					const char* keyName = "";

					if ( colHasBinding ) {
						keyName = bindings[bnd].keyName.c_str();
						ImGui::AlignTextToFramePadding();
						ImGui::TextUnformatted( keyName );
						AddTooltip( bindings[bnd].internalKeyName.c_str() );
					}

					if ( selectedBinding == BIND_ALL ) {
						// if all bindings are selected from the binding table (for clear all),
						// mark the whole first row here. otherwise, nothing is marked here,
						// as in this window only the buttons are clickble, not the key cells
						ImGui::TableSetBgColor( ImGuiTableBgTarget_CellBg, highlightRowColor );
					}

					ImGui::TableNextColumn();
					if ( colHasBinding ) {
						if ( ImGui::Button( "Rebind" ) ) {
							selState = BESS_WantBind;
							selectedBinding = bnd;
						} else {
							ImGui::SetItemTooltip( "Unbind '%s' and bind another key to %s", keyName, displayName.c_str() );
						}
						ImGui::SameLine();
						ImGui::SetCursorPosX( ImGui::GetCursorPosX() + fontSize*0.5f );
						if ( ImGui::Button( "Unbind" ) ) {
							selState = BESS_WantClear;
							selectedBinding = bnd;
						} else {
							ImGui::SetItemTooltip( "Unbind key '%s' from %s", keyName, displayName.c_str() );
						}
					} else {
						if ( ImGui::Button( "Bind" ) ) {
							selState = BESS_WantBind;
							selectedBinding = bnd;
						} else {
							ImGui::SetItemTooltip( "Set a keybinding for %s", displayName.c_str() );
						}
					}

					ImGui::PopID(); // bnd
				}

				ImGui::EndTable();
			}

			const char* addBindButtonLabel = (numBindings == 0) ? "Bind a key" : "Bind another key";
			float buttonWidth = ImGui::CalcTextSize(addBindButtonLabel).x + 2.0f * ImGui::GetStyle().FramePadding.x;
			ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - buttonWidth);

			if ( ImGui::Button( addBindButtonLabel ) ) {
				selState = BESS_WantBind;
				selectedBinding = BIND_APPEND;
			} else {
				ImGui::SetItemTooltip( "Add %s keybinding for %s",
									   (numBindings == 0) ? "a" : "another",
									   displayName.c_str() );
			}

			ImVec2 winSize = ImGui::GetWindowSize();
			ImRect winRect;
			winRect.Min = ImGui::GetWindowPos();
			winRect.Max = winRect.Min + winSize;
			ImRect workRect(viewPort.WorkPos, viewPort.WorkPos + viewPort.WorkSize);

			if ( !workRect.Contains(winRect) ) {
				// this window is at least partly outside the visible area of the screen (or SDL window)
				// => move it around so it's completely visible, if possible
				ImRect r_avoid( btnMin, btnMax );
				r_avoid.Expand( ImGui::GetStyle().ItemInnerSpacing );

				ImGuiDir dir = ImGuiDir_Right;
				ImVec2 newWinPos = ImGui::FindBestWindowPosForPopupEx( ImVec2(btnMin.x, btnMax.y),
							winSize, &dir, workRect, r_avoid, ImGuiPopupPositionPolicy_Default );
				ImVec2 posDiff = newWinPos - winPos;
				if ( fabsf(posDiff.x) > 2.0f || fabsf(posDiff.y) > 2.0f ) {
					winPos = newWinPos;
					ImGui::SetWindowPos( newWinPos );
				}
			}

			// allow closing this window with escape
			if ( ImGui::IsWindowFocused() && IsCancelKeyPressed() ) {
				showThisMenu = false;
			}
		}
		ImGui::End();

		return showThisMenu;
	}

	BindingEntrySelectionState Draw( int bindRowNum, const BindingEntrySelectionState oldSelState )
	{
		if ( IsHeading() ) {
			ImGui::SeparatorText( displayName );
			if ( description ) {
				AddDescrTooltip( description );
			}
		} else {
			ImGui::PushID( command );

			ImGui::TableNextRow( 0, ImGui::GetFrameHeightWithSpacing() );
			ImGui::TableSetColumnIndex( 0 );
			ImGui::AlignTextToFramePadding();

			// the first column (with the display name in it) gets a different background color
			ImGui::TableSetBgColor( ImGuiTableBgTarget_CellBg, displayNameBGColor );

			BindingEntrySelectionState newSelState = oldSelState;

			// not really using the selectable feature, mostly making it selectable
			// so keyboard/gamepad navigation works
			ImGui::Selectable( "##cmd", false, 0 );

			UpdateSelectionState( BIND_ALL, newSelState );

			ImGui::SameLine();
			ImGui::TextUnformatted( displayName );

			AddTooltip( command );

			if ( description ) {
				AddDescrTooltip( description );
			}

			const int numBindingColumns = imgui_numBindingColumns.GetInteger();
			int numBindings = bindings.Num();
			for ( int bnd=0; bnd < numBindingColumns ; ++bnd ) {
				ImGui::TableSetColumnIndex( bnd+1 );

				bool colHasBinding = (bnd < numBindings) && bindings[bnd].keyNum != -1;
				char selTxt[128];
				if ( colHasBinding ) {
					D3_snprintfC99( selTxt, sizeof(selTxt), "%s###%d", bindings[bnd].keyName.c_str(), bnd );
				} else {
					D3_snprintfC99( selTxt, sizeof(selTxt), "###%d", bnd );
				}
				ImGui::Selectable( selTxt, false, 0 );
				UpdateSelectionState( bnd, newSelState );

				if ( colHasBinding ) {
					AddTooltip( bindings[bnd].internalKeyName.c_str() );
				}
			}

			ImGui::TableSetColumnIndex( numBindingColumns + 1 );
			// the last column contains a "++" button that opens a window that lists all bindings
			// for this rows command (including ones not listed in the table because of lack of columns)
			// if there actually are more bindings than columns, the button is red, else it has the default color
			// clicking the button again will close the window, and the buttons color depends on whether
			// its window is open or not. only one such window can be open at at time, clicking the
			// button in another row closes the current window and opens a new one
			static int  showAllBindingsMenuRowNum = -1;
			bool allBindWinWasOpen = (showAllBindingsMenuRowNum == bindRowNum);
			int styleColorsToPop = 0;
			if ( numBindings <= numBindingColumns ) {
				if ( allBindWinWasOpen ) {
					// if the all bindings menu/window is showed for this entry,
					// the button is "active" => switch its normal and hovered colors
					ImVec4 btnColor = ImGui::GetStyleColorVec4( ImGuiCol_ButtonHovered );
					ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_Button) );
					ImGui::PushStyleColor( ImGuiCol_Button, btnColor );
					styleColorsToPop = 2;
				}
			} else { // more bindings than can be shown in the table => make ++ button red
				ImGui::PushStyleColor( ImGuiCol_Button, allBindWinWasOpen ? RedButtonHoveredColor : RedButtonColor );
				ImGui::PushStyleColor( ImGuiCol_ButtonHovered, allBindWinWasOpen ? RedButtonColor : RedButtonHoveredColor );
				ImGui::PushStyleColor( ImGuiCol_ButtonActive, RedButtonActiveColor );
				styleColorsToPop = 3;
			}

			// TODO: close the window if another row has been selected (or used to create/delete a binding or whatever)?
			bool newOpen = false;
			if ( ImGui::Button( "++" ) ) {
				showAllBindingsMenuRowNum = allBindWinWasOpen ? -1 : bindRowNum;
				newOpen = true;
				CompactBindings();
			}
			if ( ImGui::IsItemFocused() && newSelState != BESS_NotSelected ) {
				newSelState = BESS_NotSelected;
			}
			ImVec2 btnMin = ImGui::GetItemRectMin();
			ImVec2 btnMax = ImGui::GetItemRectMax();
			if ( numBindings > numBindingColumns ) {
				ImGui::SetItemTooltip( "There are additional bindings for %s.\nClick here to show all its bindings.", displayName.c_str() );
			} else {
				ImGui::SetItemTooltip( "Show all bindings for %s in a list", displayName.c_str() );
			}

			if ( styleColorsToPop > 0 ) {
				ImGui::PopStyleColor( styleColorsToPop ); // restore button colors
			}

			if ( showAllBindingsMenuRowNum == bindRowNum ) {
				ImGui::SetNextWindowBgAlpha( 1.0f );
				if ( !DrawAllBindingsWindow( newSelState, newOpen, btnMin, btnMax ) ) {
					showAllBindingsMenuRowNum = -1;
					CompactBindings();
				}
			}

			ImGui::PopID();

			if ( newSelState == BESS_NotSelected ) {
				selectedBinding = BIND_NONE;
			}
			return newSelState;
		}
		return BESS_NotSelected;
	}

	void Bind( int keyNum ) {
		if ( keyNum >= 0 ) {
			idKeyInput::SetBinding( keyNum, command );
		}
	}

	void Unbind( int keyNum ) {
		if ( keyNum >= 0 ) {
			idKeyInput::SetBinding( keyNum, "" );
		}
	}

	BindingEntrySelectionState HandleClearPopup( const char* popupName, bool newOpen )
	{
		BindingEntrySelectionState ret = BESS_WantClear;
		const int selectedBinding = this->selectedBinding;

		if ( ImGui::BeginPopupModal( popupName, NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
		{
			if ( selectedBinding == BIND_ALL ) {
				ImGui::Text( "Clear all keybindings for %s ?", displayName.c_str() );
			} else {
				ImGui::Text( "Unbind key '%s' from command %s ?",
							 bindings[selectedBinding].keyName.c_str(), displayName.c_str() );
			}

			ImGui::NewLine();
			ImGui::Text( "Press Enter (or %s) to confirm, or\nEscape (or %s) to cancel.",
						 GetGamepadBindNowButtonName(), GetGamepadCancelButtonNames() );
			ImGui::NewLine();

			// center the Ok and Cancel buttons
			float dialogButtonWidth = CalcDialogButtonWidth();
			float spaceBetweenButtons = ImGui::GetFontSize();
			float buttonOffset = (ImGui::GetWindowWidth() - 2.0f*dialogButtonWidth - spaceBetweenButtons) * 0.5f;
			ImGui::SetCursorPosX( buttonOffset );

			bool confirmedByKey = false;
			if ( !newOpen && !ImGui::IsAnyItemFocused() ) {
				// if no item is focused (=> not using keyboard or gamepad navigation to select
				//  [Ok] or [Cancel] button), check if Enter has been pressed to confirm deletion
				// (otherwise, enter can be used to chose the selected button)
				// also, don't do this when just opened, because then enter might still
				// be pressed from selecting the Unbind button in the AllBindingsWindow
				confirmedByKey = IsConfirmKeyPressed();
			}
			if ( ImGui::Button( "Ok", ImVec2(dialogButtonWidth, 0) ) || confirmedByKey ) {
				if ( selectedBinding == BIND_ALL ) {
					for ( BoundKey& bk : bindings ) {
						Unbind( bk.keyNum );
					}
					bindings.SetNum( 0, false );
					// don't select all columns after they have been cleared,
					// instead only select the first, good point to add a new binding
					this->selectedBinding = 0;
				} else {
					Unbind( bindings[selectedBinding].keyNum );
					if ( selectedBinding == imgui_numBindingColumns.GetInteger() - 1 ) {
						// when removing the binding of the last column visible
						// in the big binding table, remove that entry so
						// the next one not visible there can take its place
						RemoveBindingEntry( selectedBinding );
					} else {
						bindings[selectedBinding].Clear();
					}
				}

				ImGui::CloseCurrentPopup();
				ret = BESS_Selected;
			}
			ImGui::SetItemDefaultFocus();

			ImGui::SameLine( 0.0f, spaceBetweenButtons );
			if ( ImGui::Button( "Cancel", ImVec2(dialogButtonWidth, 0) ) || IsCancelKeyPressed() ) {
				ImGui::CloseCurrentPopup();
				ret = BESS_Selected;
			}

			ImGui::EndPopup();
		}

		return ret;
	}


	void AddKeyBinding( int keyNum )
	{
		assert( selectedBinding != -1 );
		Bind( keyNum );

		const int numBindingColumns = imgui_numBindingColumns.GetInteger();

		int numBindings = bindings.Num();
		if ( selectedBinding == BIND_ALL || selectedBinding == BIND_APPEND ) {
			for ( int i=0; i < numBindings; ++i ) {
				// if there's an empty column, use that
				if ( bindings[i].keyNum == -1 ) {
					bindings[i].Set( keyNum );
					// select the column this was inserted into
					selectedBinding = i;

					return;
				}
			}
			if ( numBindings < numBindingColumns || selectedBinding == BIND_APPEND ) {
				// just append an entry to bindings
				bindings.Append( BoundKey(keyNum) );
				selectedBinding = numBindings;
			} else {
				// insert in last column of table so it's visible
				// (but don't remove any elements from bindings!)
				bindings.Insert( BoundKey(keyNum), numBindingColumns-1 );
				selectedBinding = numBindingColumns-1;
			}
		} else {
			int selectedBinding = this->selectedBinding;
			assert( selectedBinding >= 0 );
			if ( selectedBinding < numBindings ) {
				Unbind( bindings[selectedBinding].keyNum );
				bindings[selectedBinding].Set( keyNum );
			} else  {
				if ( selectedBinding > numBindings ) {
					// apparently a column with other unset columns before it was selected
					// => add enough empty columns
					bindings.SetNum( selectedBinding, false );
				}
				bindings.Append( BoundKey(keyNum) );
			}
		}
	}

	void RemoveKeyBinding( int keyNum )
	{
		int delPos = -1;
		int numBindings = bindings.Num();
		for ( int i = 0; i < numBindings; ++i ) {
			if ( bindings[i].keyNum == keyNum ) {
				delPos = i;
				break;
			}
		}
		if ( delPos != -1 ) {
			Unbind( keyNum );
			RemoveBindingEntry( delPos );
		}
	}


	BindingEntrySelectionState HandleBindPopup( const char* popupName, bool newOpen )
	{
		BindingEntrySelectionState ret = BESS_WantBind;
		const int selectedBinding = this->selectedBinding;
		assert(selectedBinding == BIND_ALL || selectedBinding == BIND_APPEND || selectedBinding >= 0);

		ImGuiIO& io = ImGui::GetIO();

		if ( newOpen ) {
			D3::ImGuiHooks::SetKeyBindMode( true );

			// disable keyboard and gamepad input while the bind popup is open
			// (the mouse can still be used to click the Cancel button, and Escape
			//  and Gamepad Start will be handled specially as well to allow canceling)
			io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
		}

		if ( ImGui::BeginPopupModal( popupName, NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
		{
			if ( selectedBinding < 0 || selectedBinding >= bindings.Num()
				|| bindings[selectedBinding].keyNum == -1 ) {
				// add a binding
				ImGui::Text( "Press a key or button to bind to %s", displayName.c_str() );
			} else {
				// overwrite a binding
				ImGui::Text( "Press a key or button to replace '%s' binding to %s",
							  bindings[selectedBinding].keyName.c_str(), displayName.c_str() );
			}

			ImGui::NewLine();
			ImGui::TextUnformatted( "To bind a mouse button, click it in the following field" );

			const float windowWidth = ImGui::GetWindowWidth();
			ImVec2 clickFieldSize( windowWidth * 0.8f, ImGui::GetTextLineHeightWithSpacing() * 4.0f );
			ImGui::SetCursorPosX( windowWidth * 0.1f );

			ImGui::Button( "###clickField", clickFieldSize );
			bool clickFieldHovered = ImGui::IsItemHovered();

			ImGui::NewLine();
			ImGui::Text( "... or press Escape (or %s) to cancel.", GetGamepadStartName() );

			ImGui::NewLine();
			// center the Cancel button
			float dialogButtonWidth = CalcDialogButtonWidth();
			float buttonOffset = (windowWidth - dialogButtonWidth) * 0.5f;
			ImGui::SetCursorPosX( buttonOffset );

			// Note: gamepad Start also generates K_ESCAPE in DUDE
			if ( ImGui::Button( "Cancel", ImVec2(dialogButtonWidth, 0) ) || idKeyInput::IsDown( K_ESCAPE ) ) {
				ImGui::CloseCurrentPopup();
				ret = BESS_Selected;
				io.ConfigFlags |= (ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
				D3::ImGuiHooks::SetKeyBindMode( false );
			} else if ( !newOpen ) {
				// find out if any key is pressed and bind that (except for Esc which can't be
				// bound and is already handled though IsCancelKeyPressed() above)
				// (but don't run this when the popup has just been opened, because then
				//  the key that opened this, likely Enter, is still registered as pressed)

				int pressedKey = -1;

				for ( int k = 1; k < K_LAST_KEY; ++k ) {
					if ( k == K_ESCAPE || k == K_JOY_BTN_START || k == K_CONSOLE )
						continue; // unbindable keys
					if ( !clickFieldHovered && k >= K_MOUSE1 && k <= K_MOUSE3 )
						continue; // mouse buttons must be clicked within the field to bind them

					if ( idKeyInput::IsDown( k ) ) {
						pressedKey = k;
						break;
					}
				}

				if ( pressedKey != -1 ) {
					// idKeyInput::IsDown() has a special case to return true for
					// K_CTRL also when K_RIGHT_CTRL is pressed, same for K_(RIGHT_)SHIFT
					// (but not the other way around). add extra check for that, because
					// when binding keys the distinction is important.
					if ( pressedKey == K_CTRL && idKeyInput::IsDown( K_RIGHT_CTRL ) ) {
						pressedKey = K_RIGHT_CTRL;
					} else if ( pressedKey == K_SHIFT && idKeyInput::IsDown( K_RIGHT_SHIFT ) ) {
						pressedKey = K_RIGHT_SHIFT;
					}

					D3::ImGuiHooks::SetKeyBindMode( false );

					const char* oldBinding = idKeyInput::GetBinding( pressedKey );
					if ( oldBinding[0] == '\0' ) {
						// that key isn't bound yet, hooray!
						AddKeyBinding( pressedKey );
						ret = BESS_Selected;
					} else {
						// Doom3 says: already bound!
						BindingEntry* oldBE = FindBindingEntryForKey( pressedKey );
						if ( oldBE == this ) {
							// that key is already bound to this command, show warning, otherwise do nothing
							const char* keyName = GetKeyName( pressedKey );
							idStr warning = idStr::Format( "Key '%s' is already bound to this command (%s)!",
									keyName, displayName.c_str() );
							D3::ImGuiHooks::ShowWarningOverlay( warning );
							ret = BESS_Selected;
							// TODO: select column with that specific binding?
						} else {
							// that key is already bound to some other command, show confirmation popup
							rebindKeyNum = pressedKey;
							rebindOtherEntry = oldBE; // NULL for commands this menu doesn't know!

							ret = BESS_WantRebind;
						}
					}

					ImGui::CloseCurrentPopup();
					io.ConfigFlags |= (ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
				}
			}
			ImGui::EndPopup();
		}

		return ret;
	}

	BindingEntrySelectionState HandleRebindPopup( const char* popupName, bool newOpen )
	{
		BindingEntrySelectionState ret = BESS_WantRebind;

		if ( ImGui::BeginPopupModal( popupName, NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
		{
			const char* keyName = GetKeyName( rebindKeyNum );

			if ( rebindOtherEntry != nullptr ) {
				ImGui::Text( "Key '%s' is already bound to command %s !\nBind to %s instead?",
				             keyName, rebindOtherEntry->displayName.c_str(), displayName.c_str() );
			} else {
				const char* commandName = idKeyInput::GetBinding( rebindKeyNum );
				ImGui::Text( "Key '%s' is already bound to command '%s'\n (not handled by this menu)!\nBind to %s instead?",
				             keyName, commandName, displayName.c_str() );
			}

			ImGui::NewLine();
			ImGui::Text( "Press Enter (or %s) to confirm,\nor Escape (or %s) to cancel.",
						 GetGamepadBindNowButtonName(), GetGamepadCancelButtonNames() );
			ImGui::NewLine();

			// center the Ok and Cancel buttons
			float dialogButtonWidth = CalcDialogButtonWidth();
			float spaceBetweenButtons = ImGui::GetFontSize();
			float buttonOffset = (ImGui::GetWindowWidth() - 2.0f*dialogButtonWidth - spaceBetweenButtons) * 0.5f;
			ImGui::SetCursorPosX( buttonOffset );

			bool confirmedByKey = false;
			if ( !newOpen && !ImGui::IsAnyItemFocused() ) {
				// if no item is focused (=> not using keyboard or gamepad navigation to select
				//  [Ok] or [Cancel] button), check if Enter has been pressed to confirm deletion
				// (otherwise, enter can be used to chose the selected button)
				// but don't do this when just opened, because then enter might still
				// be pressed from trying to bind Enter in the BindPopup
				confirmedByKey = IsConfirmKeyPressed();
			}

			if ( ImGui::Button( "Ok", ImVec2(dialogButtonWidth, 0) ) || confirmedByKey ) {
				if ( rebindOtherEntry != nullptr ) {
					rebindOtherEntry->RemoveKeyBinding( rebindKeyNum );
				}
				// Note: AddKeyBinding() eventually calls idKeyInput::SetBinding()
				//   which implicitly unbinds the old binding when it sets the new one,
				//   so for the rebindOtherEntry == nullptr case there's nothing else to do
				AddKeyBinding( rebindKeyNum );

				rebindOtherEntry = nullptr;
				rebindKeyNum = -1;

				ImGui::CloseCurrentPopup();
				ret = BESS_Selected;
			}
			ImGui::SetItemDefaultFocus();

			ImGui::SameLine( 0.0f, spaceBetweenButtons );
			if ( ImGui::Button( "Cancel", ImVec2(dialogButtonWidth, 0) ) || IsCancelKeyPressed() ) {
				rebindOtherEntry = nullptr;
				rebindKeyNum = -1;

				ImGui::CloseCurrentPopup();
				ret = BESS_Selected;
			}

			ImGui::EndPopup();
		}

		return ret;
	}

	void HandlePopup( BindingEntrySelectionState& selectionState )
	{
		assert(selectedBinding != BIND_NONE);
		const char* popupName = nullptr;

		if ( selectionState == BESS_WantClear ) {
			if ( bindings.Num() == 0 || selectedBinding >= bindings.Num()
				|| ( selectedBinding < 0 && selectedBinding != BIND_ALL )
				|| ( selectedBinding >= 0 && bindings[selectedBinding].keyNum == -1 ) ) {
				// there are no bindings at all for this command, or at least not in the selected column
				// => don't show popup, but keep the cell selected
				selectionState = BESS_Selected;
				return;
			}
			popupName = (selectedBinding == BIND_ALL) ? "Unbind keys" : "Unbind key";
		} else if ( selectionState == BESS_WantBind ) {
			popupName = "Bind key";
		} else {
			assert( selectionState == BESS_WantRebind );
			popupName = "Confirm rebinding key";
		}

		static bool popupOpened = false;
		bool newOpen = false;
		if ( !popupOpened ) {
			ImGui::OpenPopup( popupName );
			popupOpened = true;
			newOpen = true;
		}
		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos( center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f) );

		// with the default rounding the modal popup edges don't look rounded at all
		ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 4.0f );

		BindingEntrySelectionState newSelState = BESS_Selected;
		if ( selectionState == BESS_WantClear ) {
			newSelState = HandleClearPopup( popupName, newOpen );
		} else if ( selectionState == BESS_WantBind ) {
			newSelState = HandleBindPopup( popupName, newOpen );
		} else {
			newSelState = HandleRebindPopup( popupName, newOpen );
		}

		ImGui::PopStyleVar(); // ImGuiStyleVar_WindowRounding

		if ( newSelState != selectionState ) {
			popupOpened = false;
			selectionState = newSelState;
		}
	}

};

const idDict* GetEntityDefDict( const char* name )
{
	const idDecl* decl = declManager->FindType( DECL_ENTITYDEF, name, false );
	const idDeclEntityDef* entDef = static_cast<const idDeclEntityDef*>( decl );
	return (entDef != nullptr) ? &entDef->dict : nullptr;
}

static idList<BindingEntry> bindingEntries;
static int firstObscureEntryIndex = 0;
static bool bindingsMenuAlreadyOpen = false;

// return NULL if not currently bound to anything
static BindingEntry* FindBindingEntryForKey( int keyNum )
{
	for ( BindingEntry& be : bindingEntries ) {
		for ( const BoundKey& bk : be.bindings ) {
			if ( bk.keyNum == keyNum ) {
				return &be;
			}
		}
	}
	return nullptr;
}

static void InitBindingEntries()
{
	bindingsMenuAlreadyOpen = false;

	bindingEntries.Clear();

	const BindingEntryTemplate betsMoveLookAttack[] = {
		{ "_forward",       "Forward"    , "#str_02100" },
		{ "_back",          "Backpedal"  , "#str_02101" },
		{ "_moveLeft",      "Move Left"  , "#str_02102" },
		{ "_moveRight",     "Move Right" , "#str_02103" },
		{ "_moveUp",        "Jump"       , "#str_02104" },
		{ "_moveDown",      "Crouch"     , "#str_02105" },
		{ "_left",          "Turn Left"  , "#str_02106" },
		{ "_right",         "Turn Right" , "#str_02107" },

		{ "_speed",         "Sprint"     , "#str_02109" },

		{ "_strafe",        "Strafe"     , "#str_02108" },

		{ "_lookUp",        "Look Up"    , "#str_02116" },
		{ "_lookDown",      "Look Down"  , "#str_02117" },

		{ "_impulse18",     "Center View", "#str_02119" },

		{ nullptr,          "Attack"     , "#str_02112" },

		{ "_attack",        "Attack"     , "#str_02112" },
		{ "_impulse13",     "Reload"     , "#str_02115" },
		{ "_impulse15",     "Prev. Weapon" , "#str_02113" },
		{ "_impulse14",     "Next Weapon"  , "#str_02114" },
		{ "_zoom",          "Zoom View"    , "#str_02120" },
		{ "clientDropWeapon", "Drop Weapon", "#str_04071" },

		// also the heading for weapons, but the weapons entries are generated below..
		{ nullptr,          "Weapons"    , "#str_01416" },
	};

	const BindingEntryTemplate betsOther[] = {
		{ nullptr,          "Other"          , "#str_04064" }, // TODO: or "#str_02406"	"Misc"

		{ "_impulse19",     "PDA / Score"    , "#str_04066" },
		{ "dudeSettings", "DUDE settings menu", nullptr },
		{ "savegame quick", "Quick Save"     , "#str_04067" },
		{ "loadgame quick", "Quick Load"     , "#str_04068" },
		{ "screenshot",     "Screenshot"     , "#str_04069" },
		{ "clientMessageMode",   "Chat"      , "#str_02068" },
		{ "clientMessageMode 1", "Team Chat" , "#str_02122" },
		{ "_impulse20",     "Toggle Team"    , "#str_04070" },
		{ "_impulse22",     "Spectate"       , "#str_02125" },
		{ "_impulse17",     "Ready"          , "#str_02126" },
		{ "_impulse28",     "Vote Yes"       , "#str_02127" },
		{ "_impulse29",     "Vote No"        , "#str_02128" },
		{ "_impulse40",     "Use Vehicle"    , nullptr },
	};

	int numReserve = IM_ARRAYSIZE(betsMoveLookAttack) + IM_ARRAYSIZE(betsOther);
	numReserve += 16; // up to 14 weapons + weaponheading + moveLookHeading
	numReserve += 43; // the remaining "obscure" impulses
	if(bindingEntries.NumAllocated() < numReserve) {
		bindingEntries.Resize( numReserve );
	}

	idStr moveLookHeading = GetLocalizedString( "#str_02404", "Move" );
	moveLookHeading += " / ";
	moveLookHeading += GetLocalizedString( "#str_02403", "Look" );

	bindingEntries.Append( BindingEntry( moveLookHeading ) );

	for ( const BindingEntryTemplate& bet : betsMoveLookAttack ) {
		bindingEntries.Append( BindingEntry( bet ) );
	}

	// player.def defines, in player_base, used by player_doommarine and player_doommarine_mp (and player_doommarine_ctf),
	// "def_weapon0"  "weapon_fists", "def_weapon1"  "weapon_pistol" etc
	// => get all those definitions (up to MAX_WEAPONS=16) from Player, and then
	//    get the entities for the corresponding keys ("weapon_fists" etc),
	//    which should have an entry like "inv_name"  "Pistol" (could also be #str_00100207 though!)

	// hardcorps uses: idCVar pm_character("pm_character", "0", CVAR_GAME | CVAR_BOOL, "Change Player character. 1 = Scarlet. 0 = Doom Marine");
	// but I guess (hope) they use the same weapons..
	const idDict* playerDict = GetEntityDefDict( "player_doommarine" );
	const idDict* playerDictMP = GetEntityDefDict( "player_doommarine_mp" );
	bool impulse27used = false;
	for ( int i = 0; i <= 13; ++i ) {
		int weapNum = i;
		int impulseNum = i;
		if (i == 13) {
			// Hack: D3XP uses def_weapon18 for (MP-only) weapon_chainsaw
			//  and the corresponding impulse is _impulse27
			// (otherwise def_weaponX corresponds to _impulseX)
			weapNum = 18;
			impulseNum = 27;
		}

		idStr defWeapName = idStr::Format( "def_weapon%d", weapNum );

		const char* weapName = playerDict->GetString( defWeapName, nullptr );
		if ( (weapName == nullptr || weapName[0] == '\0') && playerDictMP != nullptr ) {
			weapName = playerDictMP->GetString( defWeapName, nullptr );
		}

		// TODO: could also handle weapontoggles, in playerDict(MP):
		// "weapontoggle1"		"2,1" // _impulse1 toggles between def_weapon2 and def_weapon1
		// "weapontoggle4"		"5,4" // _impulse4 toggles between def_weapon5 and def_weapon4

		// note: weapon_PDA is skipped, because the generic open PDA action is _impulse19
		if ( weapName != nullptr && weapName[0] != '\0'
			 && idStr::Icmp( weapName, "weapon_pda" ) != 0 ) {
			const idDict* weapDict = GetEntityDefDict( weapName );
			if ( weapDict != nullptr ) {
				const char* displayName = weapDict->GetString( "inv_name", nullptr );
				if ( displayName == nullptr ) {
					displayName = weapName;
				} else if ( idStr::Cmpn( displayName, STRTABLE_ID, STRTABLE_ID_LENGTH ) == 0 ) {
					displayName = GetLocalizedString( displayName, weapName );
				}
				bindingEntries.Append( BindingEntry( idStr::Format("_impulse%d", impulseNum), displayName ) );
				if ( impulseNum == 27 )
					impulse27used = true;
			}
		}
	}

	for ( const BindingEntryTemplate& bet : betsOther ) {
		bindingEntries.Append( BindingEntry( bet ) );
	}

	firstObscureEntryIndex = bindingEntries.Num();

	bindingEntries.Append( BindingEntry( "_impulse16", "_impulse16" ) );
	bindingEntries.Append( BindingEntry( "_impulse21", "_impulse21" ) );
	// _impulse22 is "spectate", handled in "Other" section
	bindingEntries.Append( BindingEntry( "_impulse23", "_impulse23" ) );
	bindingEntries.Append( BindingEntry( "_impulse24", "_impulse24" ) );
	bindingEntries.Append( BindingEntry( "_impulse25", "_impulse25",
			"In RoE's Capture The Flag with si_midnight = 2, this appears to toggle some kind of light" ) );
	bindingEntries.Append( BindingEntry( "_impulse26", "_impulse26" ) );
	if ( !impulse27used ) {
		bindingEntries.Append( BindingEntry( "_impulse27", "_impulse27" ) );
	}
	for ( int i=28; i <= 63; ++i ) {
		if ( i == 40 ) // _impulse40 is "use vehicle", handled above in "Other" section
			continue;

		idStr impName = idStr::Format( "_impulse%d", i );
		bindingEntries.Append( BindingEntry( impName, impName ) );
	}
}

// this initialization should be done every time the bindings tab is opened,
// in case the bindings have been changed in the mean time (through console or classic menu)
static void UpdateKeyBindingsInEntries() {

	for ( BindingEntry& be : bindingEntries ) {
		be.bindings.SetNum( 0, false );
	}

	for ( int k = 1; k < K_LAST_KEY; ++k ) {
		if ( k == K_ESCAPE || k == K_JOY_BTN_START || k == K_CONSOLE ) {
			continue; // unbindable keys
		}

		const char* binding = idKeyInput::GetBinding( k );
		if ( binding[0] != '\0' ) {
			for ( BindingEntry& be : bindingEntries ) {
				if ( be.command.Icmp( binding ) == 0 ) {
					be.bindings.Append( BoundKey( k ) );
					break;
				}
			}
		}
	}
}


static void DrawBindingsMenu()
{
	if ( !bindingsMenuAlreadyOpen ) { // TODO: could go into some init function
		UpdateKeyBindingsInEntries();
		bindingsMenuAlreadyOpen = true;
	}

	ImGui::Spacing();

	int numBindingColumns = imgui_numBindingColumns.GetInteger();

	{
		// the InputInt will look kinda like this:
		// [10] [-] [+] Number of Binding columns...
		// the [-] and [+] buttons have size GetFrameHeight()^2
		// calculate the width it needs (without the "Number of ..." text)
		// for the text input field not too look too big
		const ImGuiStyle& st = ImGui::GetStyle();
		float w = ImGui::CalcTextSize("10").x;
		w += 2.0f * (ImGui::GetFrameHeight() + st.FramePadding.x + st.ItemInnerSpacing.x);
		ImGui::SetNextItemWidth(w);

		if ( ImGui::InputInt( "Number of Binding columns to show", &numBindingColumns ) ) {
			imgui_numBindingColumns.SetInteger( numBindingColumns );
		}

		numBindingColumns = idMath::ClampInt( 1, 10, numBindingColumns );

		// calculate the background color for the first column of the key binding tables
		// (it contains the command, while the other columns contain the keys bound to that command)
		displayNameBGColor = ImGui::GetColorU32( ImGuiCol_TableHeaderBg, 0.5f );


		if ( ImGui::TreeNode("Usage Help") ) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);

			ImGui::TextWrapped( "Double click a keybinding entry below to bind a (different) key, or select it by clicking it once or navigating to it with cursor keys or gamepad and pressing Enter (or %s) to (re)bind it.",
			                    GetGamepadBindNowButtonName() );
			ImGui::TextWrapped( "Remove a key binding (unbind) by selecting it and pressing Backspace or Delete (or %s).",
			                    GetGamepadUnbindButtonName() );
			ImGui::TextWrapped( "If you select the first column (with the command name), you can unbind all keybindings for that command, or add another keybinding for it without overwriting an existing one." );
			ImGui::TextWrapped( "You can unselect the currently selected binding by clicking it again or by pressing Escape (or %s).",
			                    GetGamepadCancelButtonNames() );
			ImGui::TextWrapped( "The [++] button on the right opens (or closes) a window that shows all keys bound to the corresponding command (even if it's more than the number of binding columns) and has buttons to configure them. It's red when there actually are more key bound than can be shown in the columns of this window." );

			ImGui::PopStyleColor(); // ImGuiCol_Text
			ImGui::TreePop();
		} else {
			AddTooltip( "Click to show help text" );
		}
	}

	static int selectedRow = -1;
	static BindingEntrySelectionState selectionState = BESS_NotSelected;

	// make the key column entries in the bindings table center-aligned instead of left-aligned
	ImGui::PushStyleVar( ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.0f) );

	ImU32 borderCol = ImGui::GetColorU32( ImGuiCol_TableBorderLight, 0.5f );
	ImGui::PushStyleColor( ImGuiCol_TableBorderLight, borderCol );

	ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;

	// inTable: are we currently adding elements to a table of bindings?
	//  (we're not when adding a heading from bindingEntries: existing tables are ended
	//   before a heading and a new table is started afterwards)
	bool inTable = false;
	// did the last ImGui::BeginTable() call return true (or is it not currently visible)?
	// (init to true so the first heading before any bindings is shown)
	bool lastBeginTable = true;
	int tableNum = 1;

	const ImVec2 defFramePadding = ImGui::GetStyle().FramePadding;
	const float commandColumnWidth = ImGui::CalcTextSize( "DUDE settings menu" ).x + defFramePadding.x * 2.0f;
	const float overflowColumnWidth = ImGui::CalcTextSize( "++" ).x + defFramePadding.x * 2.0f;

	// this handles the "regular" binding entries, i.e. everything but the "obscure" impulses list
	for ( int i=0, n=firstObscureEntryIndex; i < n; ++i ) {
		BindingEntry& be = bindingEntries[i];
		bool isHeading = be.IsHeading();
		if ( !isHeading && !inTable ) {
			// there is no WIP table (that has been started but not ended yet)
			// and the current element is a regular bind entry that's supposed
			// to go in a table, so start a new one
			inTable = true;
			char tableID[10];
			D3_snprintfC99( tableID, sizeof(tableID), "bindTab%d", tableNum++ );
			lastBeginTable = ImGui::BeginTable( tableID, numBindingColumns + 2, tableFlags );
			if ( lastBeginTable ) {
				ImGui::TableSetupScrollFreeze(1, 0);
				ImGui::TableSetupColumn( "Command", ImGuiTableColumnFlags_WidthFixed, commandColumnWidth );
				for ( int j=0; j < numBindingColumns; ++j ) {
					char colName[16];
					D3_snprintfC99(colName, sizeof(colName), "binding%d", j);
					ImGui::TableSetupColumn( colName );
				}

				ImGui::TableSetupColumn( "ShowAll", ImGuiTableColumnFlags_WidthFixed, overflowColumnWidth );
			}
		} else if ( isHeading && inTable ) {
			// we've been adding elements to a table (unless lastBeginTable = false) that hasn't
			// been closed with EndTable() yet, but now we're at a heading so the table is done
			if ( lastBeginTable ) {
				ImGui::EndTable();
			}
			inTable = false;
		}

		if ( lastBeginTable ) { // if ImGui::BeginTable() returned false, don't draw its elements
			BindingEntrySelectionState bess = (selectedRow == i) ? selectionState : BESS_NotSelected;
			bess = be.Draw( i, bess );
			if ( bess != BESS_NotSelected ) {
				selectedRow = i;
				selectionState = bess;
			} else if ( selectedRow == i ) {
				// this row was selected, but be.Draw() returned BESS_NotSelected, so unselect it
				selectedRow = -1;
			}
		}
	}
	if ( inTable && lastBeginTable ) {
		// end the last binding table, if any
		ImGui::EndTable();
	}

	const int numBindingEntries = bindingEntries.Num();
	// now the obscure impulses, unless they're hidden
	bool showObscImp = ImGui::CollapsingHeader( "Obscure Impulses" );
	AddDescrTooltip( "_impulseXY commands that are usually unused, but might be used by some mods, e.g. for additional weapons" );
	if (showObscImp) {
		if ( ImGui::BeginTable( "bindTabObsc", numBindingColumns + 2, tableFlags ) ) {
			ImGui::TableSetupScrollFreeze(1, 0);
			ImGui::TableSetupColumn( "Command", ImGuiTableColumnFlags_WidthFixed, commandColumnWidth );
			for ( int j=0; j < numBindingColumns; ++j ) {
				char colName[16];
				D3_snprintfC99(colName, sizeof(colName), "binding%d", j);
				ImGui::TableSetupColumn( colName );
			}

			ImGui::TableSetupColumn( "ShowAll", ImGuiTableColumnFlags_WidthFixed, overflowColumnWidth );

			for ( int i=firstObscureEntryIndex; i < numBindingEntries; ++i ) {
				BindingEntry& be = bindingEntries[i];
				BindingEntrySelectionState bess = (selectedRow == i) ? selectionState : BESS_NotSelected;
				bess = be.Draw( i, bess );
				if ( bess != BESS_NotSelected ) {
					selectedRow = i;
					selectionState = bess;
				} else if ( selectedRow == i ) {
					// this row was selected, but be.Draw() returned BESS_NotSelected, so unselect it
					selectedRow = -1;
				}
			}

			ImGui::EndTable();
		}
	}

	if ( ImGui::IsWindowFocused() && IsCancelKeyPressed() ) {
		// pressing Escape unfocuses whatever entry is focused
		selectedRow = -1;
		// FIXME: the following looks dangerous, but for some reason ImGui doesn't
		//        seem to have a function to unfocus o_O
		ImGui::GetCurrentContext()->NavId = 0;
	}

	ImGui::PopStyleColor(); // ImGuiCol_TableBorderLight
	ImGui::PopStyleVar(); // ImGuiStyleVar_SelectableTextAlign

	// WantBind or WantClear or WantRebind => show a popup
	if ( selectionState > BESS_Selected ) {
		assert(selectedRow >= 0 && selectedRow < numBindingEntries);
		bindingEntries[selectedRow].HandlePopup( selectionState );
	}
}



/****************************
 *                          *
 * Other (CVar-based) Menus *
 *                          *
 ****************************/

static void AddCVarOptionTooltips( const idCVar& cvar, const char* desc = nullptr )
{
	AddTooltip( cvar.GetName() );
	AddDescrTooltip( desc ? desc : cvar.GetDescription() );
}

enum OptionType {
	OT_NONE,
	OT_HEADING, // not an option, just a heading on the page
	OT_BOOL,
	OT_FLOAT,
	OT_INT,
	OT_CUSTOM,  // using a callback in Draw()
	OT_GROUP,   // a collapsible "burger" group that owns a child array of options
};

// forward decls so CVarOption's inline Init()/Draw() can recurse into groups (the free
// functions and the group-chrome helpers below are defined further down)
struct CVarOption;
static void InitOptions( CVarOption options[], int numOptions );
static void DrawOptions( CVarOption options[], int numOptions );
static bool BeginSettingsGroup( const char* label, idCVar* masterCvar, const char* masterTooltip, bool gateBody );
static void EndSettingsGroup();

struct CVarOption {
	typedef void (*DrawCallback)( idCVar& cvar ); // for OT_CUSTOM

	const char* name = nullptr;
	idCVar* cvar = nullptr;
	const char* label = nullptr;
	DrawCallback drawCallback = nullptr;
	OptionType type = OT_NONE;
	// TODO: the following two could be a union, together with drawCallback and possibly others!
	float minVal = 0.0f;
	float maxVal = 0.0f;
	// OT_GROUP only: the child options this group owns, plus an optional tooltip for the
	// group's master switch (name holds the master cvar, or is null for a plain category)
	CVarOption* children = nullptr;
	int numChildren = 0;
	const char* tooltip = nullptr;


	CVarOption() = default;

	CVarOption(const char* _name, const char* _label, OptionType _type, float _minVal = 0.0f, float _maxVal = 0.0f)
	: name(_name), label(_label), type(_type), minVal(_minVal), maxVal(_maxVal)
	{}

	CVarOption(const char* _name, DrawCallback drawCB)
	: name(_name), drawCallback(drawCB), type(OT_CUSTOM)
	{}

	CVarOption(const char* headingLabel) : label(headingLabel), type(OT_HEADING)
	{}

	// collapsible "burger" group. _masterCvar (may be null) draws a switch on the header
	// row that turns the whole group on/off; the body greys out while it's off.
	CVarOption(const char* _label, const char* _masterCvar, CVarOption* _children, int _numChildren, const char* _tip = nullptr)
	: name(_masterCvar), label(_label), type(OT_GROUP), children(_children), numChildren(_numChildren), tooltip(_tip)
	{}

	void Init()
	{
		if (name != NULL) {
			cvar = cvarSystem->Find(name);
		}
		if (type == OT_GROUP && children != nullptr) {
			InitOptions(children, numChildren);
		}
	}

	void Draw()
	{
		if (type == OT_GROUP) {
			DrawGroup();
		} else if (type == OT_HEADING) {
			if (label != NULL) {
				ImGui::SeparatorText( label );
			}
		} else if (cvar != nullptr) {
			switch(type) {
				case OT_BOOL:
				{
					bool b = cvar->GetBool();
					bool bOrig = b;
					ImGui::Checkbox( label, &b );
					AddCVarOptionTooltips( *cvar );
					if (b != bOrig) {
						cvar->SetBool(b);
					}
					break;
				}
				case OT_FLOAT:
				{
					float f = cvar->GetFloat();
					float fOrig = f;
					// TODO: make format configurable?
					ImGui::SliderFloat(label, &f, minVal, maxVal, "%.2f", 0);
					AddCVarOptionTooltips( *cvar );
					if(f != fOrig) {
						cvar->SetFloat(f);
					}
					break;
				}
				case OT_INT:
				{
					int i = cvar->GetInteger();
					int iOrig = i;
					ImGui::SliderInt(label, &i, minVal, maxVal);
					AddCVarOptionTooltips( *cvar );
					if (i != iOrig) {
						cvar->SetInteger(i);
					}
					break;
				}
				case OT_CUSTOM:
					if (drawCallback != nullptr) {
						drawCallback(*cvar);
					}
					break;
			}
		}
	}

	// OT_GROUP is drawn separately: it may have no master cvar, so it can't sit inside the
	// `cvar != nullptr` branch above.
	void DrawGroup()
	{
		if ( BeginSettingsGroup( label, cvar, tooltip, true ) ) {
			DrawOptions( children, numChildren );
			EndSettingsGroup();
		}
	}
};

static void InitOptions(CVarOption options[], int numOptions)
{
	for( int i=0; i < numOptions; ++i ) {
		options[i].Init();
	}
}

static void DrawOptions(CVarOption options[], int numOptions)
{
	for( int i=0; i < numOptions; ++i ) {
		options[i].Draw();
	}
}

// ---- Collapsible "burger" settings groups ------------------------------------------
// A framed, collapsible section header. When masterCvar is non-null a checkbox bound to
// it sits at the right edge of the header row - the whole group's on/off switch. With
// gateBody=true the body is greyed out (but stays visible) while the switch is off, so
// the tuning controls remain discoverable. Returns true when the body is open; the caller
// then draws the child widgets and, ONLY if it returned true, calls EndSettingsGroup().
// Groups start expanded and remember their fold state (keyed by label).
static bool BeginSettingsGroup( const char* label, idCVar* masterCvar = nullptr,
                                const char* masterTooltip = nullptr, bool gateBody = true )
{
	ImGui::PushID( label );

	if ( masterCvar != nullptr ) {
		ImGui::SetNextItemAllowOverlap();
	}
	const bool open = ImGui::CollapsingHeader( label, ImGuiTreeNodeFlags_AllowOverlap );

	if ( masterCvar != nullptr ) {
		// right-align the master switch on the header row (over the framed header), with a
		// little breathing room from the right edge / scrollbar.
		const ImGuiStyle& st = ImGui::GetStyle();
		ImGui::SameLine();
		ImGui::SetCursorPosX( ImGui::GetWindowWidth() - ImGui::GetFrameHeight()
			- st.FramePadding.x - st.ItemSpacing.x - st.ScrollbarSize );
		bool on = masterCvar->GetBool();
		if ( ImGui::Checkbox( "##groupmaster", &on ) ) {
			masterCvar->SetBool( on );
		}
		// Attach the description straight to the switch (hover it to read). We deliberately
		// do NOT use the "(?)" marker here: the switch sits at the right edge, so a trailing
		// "(?)" drawn after it via SameLine would bleed off-screen.
		if ( ImGui::BeginItemTooltip() ) {
			ImGui::PushTextWrapPos( ImGui::GetFontSize() * 35.0f );
			ImGui::TextDisabled( "%s", masterCvar->GetName() );
			ImGui::TextUnformatted( masterTooltip != nullptr ? masterTooltip : masterCvar->GetDescription() );
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
	}

	if ( !open ) {
		ImGui::PopID();
		return false;
	}

	ImGui::Indent();
	ImGui::BeginDisabled( gateBody && masterCvar != nullptr && !masterCvar->GetBool() );
	return true;
}

static void EndSettingsGroup()
{
	ImGui::EndDisabled();
	ImGui::Unindent();
	ImGui::PopID();
	ImGui::Spacing();
}

static CVarOption controlOptions[] = {

	CVarOption("Mouse Settings"),
	CVarOption("sensitivity", "Sensitivity", OT_FLOAT, 0.01f, 30.0f),
	CVarOption("m_smooth", "Smoothing Samples", OT_INT, 1, 8),
	CVarOption("in_nograb", "Don't grab Mouse Cursor (for debugging/testing)", OT_BOOL),
	CVarOption("m_invertLook", [](idCVar& cvar) {
			int val = cvar.GetInteger();
			if ( ImGui::Combo( "Invert Mouse Look", &val, "Don't Invert Mouse\0Invert Up/Down\0Invert Left/Right\0Invert Both Directions\0" ) ) {
				cvar.SetInteger( val );
			}
			AddCVarOptionTooltips( cvar, "If you played too many flight sims.." );
		} ),

	CVarOption("Keyboard Settings"),
	CVarOption("in_grabKeyboard", "Grab Keyboard", OT_BOOL),
	CVarOption("in_ignoreConsoleKey", "Don't open console with key between Esc, Tab and 1", OT_BOOL),

	CVarOption("Gamepad Settings"),
	CVarOption("in_useGamepad", "Enable Gamepad Support", OT_BOOL),
	CVarOption("joy_gamepadLayout", [](idCVar& cvar) {
			int sel = cvar.GetInteger() + 1; // -1 .. 3 => 0 .. 4
			int selOrig = sel;
			// -1: auto (needs SDL 2.0.12 or newer), 0: XBox-style, 1: Nintendo-style, 2: PS4/5-style, 3: PS2/3-style
			const char* items[] = { "Auto-Detect", "XBox Controller-like",
					"Nintendo-style", "Playstation 4/5 Controller-like",
					"Playstation 2/3 Controller-like" };
			ImGui::Combo("Gamepad Layout", &sel, items, IM_ARRAYSIZE(items));
			AddCVarOptionTooltips( cvar, "Button Layout of Gamepad (esp. for the displayed names of the 4 buttons on the right)" );
			if(sel != selOrig) {
				cvar.SetInteger(sel-1);
			}
		}),
	CVarOption("joy_deadZone", "Axis Deadzone", OT_FLOAT, 0.0f, 0.99f),
	CVarOption("joy_triggerThreshold", "Trigger Threshold/Deadzone", OT_FLOAT, 0.0f, 0.99f),
	CVarOption("joy_pitchSpeed", "Pitch speed (for looking up/down)", OT_INT, 60.0f, 600.0f),
	CVarOption("joy_yawSpeed", "Yaw speed (for looking left/right)", OT_INT, 60.0f, 600.0f),
	// TODO: joy_invertLook? (I don't really see the point, one can just bind move stick up to look down)
	CVarOption("joy_gammaLook", "Use logarithmic gamma curve instead of power curve for axes", OT_BOOL),
	CVarOption("joy_powerScale", "If using power curve, this is the exponent", OT_FLOAT, 0.1f, 10.0f), // TODO: what are sensible min/max values?
	// TODO: joy_dampenlook and joy_deltaPerMSLook ? comment in code says they were "bad idea"
};

struct VidMode {
	idStr label;
	int width;
	int height;
	int mode;

	VidMode() : width(0), height(0), mode(0) {}

	VidMode( int w, int h, int m ) : width(w), height(h), mode(m)  {}

	void Init() {
		if ( mode == -1 ) {
			label = "Custom";
		} else {
			label = idStr::Format( "%4d x %d", width, height );
		}
	}
};

// NOTE: the former Video Options tab (resolution, window mode, VSync, brightness/gamma,
// anisotropic filtering, textures, GUI/layout, screenshots, OpenGL info, ...) has been folded
// into the Graphics tab. Soft Particles / Depth Buffer Capture live in enhancementOptions[].
// The vanilla "Advanced Options" toggles (r_skipNewAmbient, r_shadows, r_skipSpecular,
// r_skipBump) were removed 2026-08-02 - every supported GPU runs them on; still console-reachable.

// Screenshots section — a plain section at the bottom of the Graphics tab.
static CVarOption screenshotOptions[] = {
	CVarOption( "Screenshots" ),
	CVarOption( "r_screenshotFormat", []( idCVar& cvar ) {
		// "Screenshot format. 0 = TGA (default), 1 = BMP, 2 = PNG, 3 = JPG"
		int curFormat = idMath::ClampInt( 0, 3, cvar.GetInteger() );
		if ( ImGui::Combo( "Screenshot Format", &curFormat, "TGA\0BMP\0PNG\0JPG\0" ) ) {
			cvar.SetInteger( curFormat );
		}
		AddTooltip( "r_screenshotFormat" );
	} ),
	CVarOption( "r_screenshotPngCompression", "Compression level for PNG screenshots", OT_INT, 0, 9 ),
	CVarOption( "r_screenshotJpgQuality", "Quality level for JPG screenshots", OT_INT, 1, 100 ),
};

// Brightness / Gamma (live), shown in the Display group.
static CVarOption displayColorOptions[] = {
	CVarOption( "r_brightness", "Brightness", OT_FLOAT, 0.5f, 2.0f ),
	CVarOption( "r_gamma", "Gamma", OT_FLOAT, 0.5f, 3.0f ),
	CVarOption( "r_gammaInShader", "Apply gamma and brightness in shaders", OT_BOOL ),
};

// Anisotropic filtering (live) — its own collapsible group in the Graphics tab.
static CVarOption anisoOptions[] = {
	CVarOption( "image_anisotropy", []( idCVar& cvar ) {
		const char* descr = "Max Texture Anisotropy";
		if ( glConfig.maxTextureAnisotropy > 1 )
		{
			int texAni = cvar.GetInteger();
			const char* fmtStr = (texAni > 1) ? "%d" : "No Anisotropic Filtering";
			ImGui::SliderInt( "Level", &texAni, 1,
			                  glConfig.maxTextureAnisotropy, fmtStr,
			                  ImGuiSliderFlags_AlwaysClamp );
			if ( texAni != cvar.GetInteger() ) {
				cvar.SetInteger( texAni );
			}
		} else {
			ImGui::BeginDisabled();
			int texAni = 0;
			ImGui::SliderInt( "Level", &texAni, 1, 8, "Not supported" );
			ImGui::EndDisabled();
			descr = "Anisotropic filtering is not supported by this system!";
		}
		AddCVarOptionTooltips( cvar, descr );
	} ),
};

// GUI / layout toggles (live). Drawn as a plain section
// (not a burger) near the bottom of the Graphics tab, just above Screenshots.
static CVarOption guiLayoutOptions[] = {
	CVarOption( "GUI and Layout" ),
	CVarOption( "r_scaleMenusTo43", "Scale fullscreen menus to 4:3", OT_BOOL ),
	CVarOption( "gui_hiResFonts", "High-resolution GUI fonts (displays taller than 720p)", OT_BOOL ),
};

// Non-vanilla graphical enhancements. This tab (and everything in it) is only
// shown/active on backends that support it (GL3/Vulkan) - see
// R_BackendSupportsEnhancements(). The legacy ARB2 renderer stays faithful to
// vanilla Doom 3, so none of these effects run there.
// The Graphics tab is organised into collapsible "burger" groups (see OT_GROUP /
// BeginSettingsGroup). Each group owns a child array below; the top-level enhancementOptions[]
// wires them together. Groups with a single clean master toggle (Tessellation, Parallax)
// put that switch on the header row and grey their body out while off; the rest are plain
// collapsible categories. The Antialiasing, Ambient Occlusion, Reflections and Shadows
// sections use custom widgets (discrete-stop sliders, backend notes) and are hand-drawn as
// groups directly in DrawGraphicsMenu().

static CVarOption lightingOptions[] = {
	CVarOption( "r_pbr", []( idCVar& cvar ) {
		bool enable = cvar.GetBool();
		if ( ImGui::Checkbox( "PBR Materials (GGX)", &enable ) ) {
			cvar.SetBool( enable );
		}
		const char* descr = "Physically based specular (GGX) with energy-conserving diffuse. Replaces the\n"
			"specular shading model below while enabled (those settings are kept, just\n"
			"inactive). Currently uses global roughness defaults; per-material values arrive\n"
			"with the material classifier. Pairs well with HDR (recommended, not required).\n"
			"The strongest deviation from the vanilla look in this tab.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
	// the three specular-model controls below are superseded by the PBR path, so
	// they grey out while r_pbr is on; their values are preserved untouched so
	// toggling PBR back off restores the exact previous look.
	CVarOption( "r_shading", []( idCVar& cvar ) {
		ImGui::BeginDisabled( r_pbr.GetBool() );
		int sel = idMath::ClampInt( 0, 1, cvar.GetInteger() );
		if ( ImGui::Combo( "Specular Shading Model", &sel, "Vanilla (lookup table)\0Blinn-Phong\0" ) ) {
			cvar.SetInteger( sel );
		}
		const char* descr = "Vanilla reproduces the classic Doom 3 specular highlight exactly.\nBlinn-Phong is an analytic model tuned by Specular Exponent.";
		AddCVarOptionTooltips( cvar, descr );
		ImGui::EndDisabled();
	} ),
	CVarOption( "r_specularScale", []( idCVar& cvar ) {
		ImGui::BeginDisabled( r_pbr.GetBool() );
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Specular Scale", &f, 0.0f, 8.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( f );
		}
		AddCVarOptionTooltips( cvar );
		ImGui::EndDisabled();
	} ),
	CVarOption( "r_specularExp", []( idCVar& cvar ) {
		ImGui::BeginDisabled( r_pbr.GetBool() );
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Specular Exponent (Blinn-Phong)", &f, 1.0f, 128.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( f );
		}
		AddCVarOptionTooltips( cvar );
		ImGui::EndDisabled();
	} ),
	// light flare/glare deform sprites (the "bloom" around some lights). Vanilla
	// value is 1.0; lower to dampen, 0 to switch them off. Works on all backends
	// but lives here as the deviate-from-vanilla knob.
	CVarOption( "r_flareSize", "Light Flare / Glare Size", OT_FLOAT, 0.0f, 2.0f ),
	CVarOption( "r_emissiveSurfaces", []( idCVar& cvar ) {
		bool enable = cvar.GetBool();
		if ( ImGui::Checkbox( "Emissive Surfaces Cast Light", &enable ) ) {
			cvar.SetBool( enable );
		}
		const char* descr = "GUI screens, monitors and video panels glow but cast no light in vanilla Doom 3,\n"
			"so they read as decals pasted on an unlit wall. This lets them bleed a small,\n"
			"content-tinted fill light onto nearby geometry. Fine-tuning lives in the Debugging tab.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
	CVarOption( "r_itemGlow", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Self-lit Items Glow", &f, 0.0f, 1.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( idMath::ClampFloat( 0.0f, 1.0f, f ) );
		}
		const char* descr = "Pickups with glowing parts (armor, medkits, ammo lights, powerups) give off a very\n"
			"faint light of their glow's colour, so they're findable in pitch-black rooms.\n"
			"0 = vanilla (no light), 1 = full faint glow. Only noticeable in near-total darkness.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
};

// NOTE: the Shadows section is hand-drawn in DrawGraphicsMenu() (a burger group under
// its master toggle, with a fine-grained bias control), not listed here.

// Tessellation: the master switch (r_tessellation) rides the group header, so these
// sub-controls no longer gate themselves - the group greys them out while it is off.
static CVarOption tessellationOptions[] = {
	CVarOption( "r_tessLevel", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Tessellation Level", &f, 1.0f, 16.0f, "%.0f", 0 ) ) {
			cvar.SetFloat( f );
		}
		AddCVarOptionTooltips( cvar );
	} ),
	CVarOption( "r_tessDisplace", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Displacement", &f, -4.0f, 4.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( f );
		}
		const char* descr = "Normal-map displacement: push surface detail along the normal by this many world\n"
			"units. Doom 3 has no runtime height maps, so height is approximated from the bump\n"
			"map, positive raises detail, negative carves it in. 0 = pure PN silhouette smoothing.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
	CVarOption( "r_tessMaxDist", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Tessellation Distance", &f, 0.0f, 2048.0f, "%.0f", 0 ) ) {
			cvar.SetFloat( f );
		}
		const char* descr = "View distance (world units) beyond which tessellation rolls back toward flat,\n"
			"an LOD/performance guard. 0 = full tessellation at any distance.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
};

// Parallax: master switch (r_parallax) rides the group header.
static CVarOption parallaxOptions[] = {
	CVarOption( "r_parallaxScale", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Parallax Depth", &f, 0.0f, 2.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( idMath::ClampFloat( 0.0f, 4.0f, f ) );
		}
		const char* descr = "Strength of the parallax relief. 0 = flat, higher exaggerates the apparent depth.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
	CVarOption( "r_parallaxShadow", []( idCVar& cvar ) {
		float f = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Parallax Self-Shadow", &f, 0.0f, 1.0f, "%.2f", 0 ) ) {
			cvar.SetFloat( idMath::ClampFloat( 0.0f, 1.0f, f ) );
		}
		const char* descr = "The parallax relief casts soft contact shadows from the light direction, which is\n"
			"what makes the fake depth read as real. 0 = off. Adds a second (half-step) march\n"
			"per lit pixel.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
};

// Post-Processing: a plain collapsible category (no single master toggle).
static CVarOption postProcessOptions[] = {
	// HDR rendering: accumulate the scene into a float (RGBA16F) buffer instead of the
	// 8-bit backbuffer, then resolve back. Removes fog/gradient banding.
	CVarOption( "r_hdr", "HDR Rendering", OT_BOOL ),
	// Tonemap curve: needs the float scene buffer, so it greys out while HDR is off.
	// 0 = faithful (straight resolve, the default); the curves are opt-in cinematic looks.
	CVarOption( "r_hdrTonemap", []( idCVar& cvar ) {
		ImGui::BeginDisabled( !r_hdr.GetBool() );
		int sel = idMath::ClampInt( 0, 4, cvar.GetInteger() );
		if ( ImGui::Combo( "Tonemap", &sel, "Off (faithful)\0Reinhard\0ACES\0AgX\0Khronos PBR Neutral\0" ) ) {
			cvar.SetInteger( sel );
		}
		const char* descr = "Compress the HDR scene's bright highlights into the display range with a filmic\n"
			"curve, instead of hard-clipping them to white. Off reproduces the vanilla resolve\n"
			"exactly; ACES is the game-standard look, AgX and PBR Neutral shift colour the least.\n"
			"Exposure below feeds the curve (it does nothing while Tonemap is Off).";
		AddCVarOptionTooltips( cvar, descr );

		// Exposure knob, right under the curve dropdown. Exposure only shapes the tonemap
		// curves (mode 0 stays a pure passthrough), so it greys out when Tonemap is Off.
		ImGui::BeginDisabled( sel == 0 );
		float exposure = cvarSystem->GetCVarFloat( "r_hdrExposure" );
		if ( ImGui::SliderFloat( "Exposure", &exposure, 0.1f, 8.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrExposure", exposure );
		}
		AddTooltip( "r_hdrExposure: linear exposure multiplier applied before the tonemap curve.\n"
			"Raises or lowers the scene brightness feeding the curve; default 1.25. No effect while Tonemap is Off." );

		// Emissive overbright, same tonemap-gated block. Pushes additive self-illum
		// surfaces above 1.0 so the curve rolls them off as real highlights; like
		// exposure it only acts with a curve on, so it greys out when Tonemap is Off.
		float overbright = cvarSystem->GetCVarFloat( "r_hdrOverbright" );
		if ( ImGui::SliderFloat( "Emissive Overbright", &overbright, 1.0f, 8.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrOverbright", overbright );
		}
		AddTooltip( "r_hdrOverbright: multiply additive self-illum surfaces (lamps, screens, fire, glares)\n"
			"so they exceed 1.0 and the tonemap curve treats them as real highlights (and eye-adaptation\n"
			"has bright anchors). 1 = off; ~2 is a good look. No effect while Tonemap is Off." );
		ImGui::EndDisabled();

		ImGui::EndDisabled();
	} ),
	CVarOption( "r_postFilmGrain", "Film Grain", OT_FLOAT, 0.0f, 0.25f ),
	CVarOption( "r_postFilmGrainSize", "Film Grain Size", OT_FLOAT, 1.0f, 4.0f ),
	CVarOption( "r_postChromaticAberration", "Chromatic Aberration", OT_FLOAT, 0.0f, 0.5f ),
};

// Particles: a plain collapsible category (soft particles + depth capture + smoke blend
// each have their own toggle / dependency handling).
static CVarOption particleOptions[] = {
	CVarOption( "r_useSoftParticles", []( idCVar& cvar ) {
		bool enable = cvar.GetBool();
		if ( ImGui::Checkbox( "Use Soft Particles", &enable ) ) {
			cvar.SetBool( enable );
			if ( enable && r_enableDepthCapture.GetInteger() == 0 ) {
				r_enableDepthCapture.SetInteger(-1);
				D3::ImGuiHooks::ShowWarningOverlay( "Capturing the Depth Buffer was disabled.\nEnabled it because soft particles need it!" );
			}
		}
		const char* descr = "! Can slow down rendering !\nSoften particle transitions when player walks through them or they cross solid geometry. Needs r_enableDepthCapture.";
		AddCVarOptionTooltips( cvar, descr );
	} ),
	CVarOption( "r_enableDepthCapture", []( idCVar& cvar ) {
			int sel = idMath::ClampInt( -1, 1, cvar.GetInteger() ) + 1; // +1 for -1..1 to 0..2
			if ( ImGui::Combo( "Capture Depth Buffer to Texture", &sel, "Auto (enable if needed for Soft Particles)\0Disabled\0Always Enabled\0" ) ) {
				--sel; // back to -1..1 from 0..2
				cvar.SetInteger( sel );
				if ( sel == 0 && r_useSoftParticles.GetBool() ) {
					r_useSoftParticles.SetBool( false );
					D3::ImGuiHooks::ShowWarningOverlay( "You disabled capturing the Depth Buffer.\nDisabling Soft Particles because they need the depth buffer texture." );
				}
			}
			AddCVarOptionTooltips( cvar );
		}),
	// Smoke darkness blend: fade alpha-blended smoke/fog into scene darkness.
	// Builds on soft particles (same captured-scene path), so enabling it turns
	// them on too. The two curve knobs are grouped and greyed out while it's off.
	CVarOption( "r_smokeDarkBlend", []( idCVar& cvar ) {
		bool enable = cvar.GetBool();
		if ( ImGui::Checkbox( "Smoke Blends Into Darkness", &enable ) ) {
			cvar.SetBool( enable );
			if ( enable && !r_useSoftParticles.GetBool() ) {
				r_useSoftParticles.SetBool( true );
				if ( r_enableDepthCapture.GetInteger() == 0 ) {
					r_enableDepthCapture.SetInteger( -1 );
				}
				D3::ImGuiHooks::ShowWarningOverlay( "Enabled Soft Particles (and depth capture).\nSmoke darkness blend builds on them." );
			}
		}
		const char* descr = "Fade smoke, steam and dust into shadow: where the scene behind a puff is black it shows\n"
			"at only the 'Darkness Floor' opacity below, ramping to fully visible once the background\n"
			"reaches the 'Full-opacity Light Level'. Covers both alpha-blended and additive smoke (e.g.\n"
			"Doom 3's smokepuff steam); fire, sparks and glares are left bright (matched by material name).\n"
			"Builds on Soft Particles (turned on automatically). opengl3/Vulkan only.";
		AddCVarOptionTooltips( cvar, descr );

		ImGui::BeginDisabled( !cvar.GetBool() );
		float floorPct = r_smokeDarkBlendFloor.GetFloat() * 100.0f;
		if ( ImGui::SliderFloat( "Darkness Floor", &floorPct, 0.0f, 100.0f, "%.0f%%" ) ) {
			r_smokeDarkBlendFloor.SetFloat( idMath::ClampFloat( 0.0f, 1.0f, floorPct / 100.0f ) );
		}
		AddTooltip( "How visible smoke stays over a fully black background, as a percentage of its normal "
			"opacity. 15% is barely visible; 100% = no dimming." );
		float kneePct = r_smokeDarkBlendKnee.GetFloat() * 100.0f;
		if ( ImGui::SliderFloat( "Full-opacity Light Level", &kneePct, 5.0f, 100.0f, "%.0f%%" ) ) {
			r_smokeDarkBlendKnee.SetFloat( idMath::ClampFloat( 0.05f, 1.0f, kneePct / 100.0f ) );
		}
		AddTooltip( "Background brightness at which smoke returns to full opacity. Lower = smoke recovers "
			"quickly with just a little light; higher = stays dim except in bright areas." );
		ImGui::EndDisabled();
	} ),
};

// Top-level Enhancements groups. A non-null master cvar (2nd arg) draws the group's
// on/off switch on the header row and greys the body while off; nullptr = a plain
// collapsible category. Init()/DrawOptions() recurse into each group's child array.
static CVarOption enhancementOptions[] = {
	CVarOption( "Lighting", nullptr, lightingOptions, IM_ARRAYSIZE( lightingOptions ) ),
	CVarOption( "Tessellation (only Vulkan)", "r_tessellation", tessellationOptions, IM_ARRAYSIZE( tessellationOptions ),
		"GPU PN-triangle tessellation that rounds the low-poly silhouettes of animated\n"
		"characters (enemies, NPCs). Vulkan only - the OpenGL backend's GL 3.3 context has\n"
		"no tessellation stages, so this does nothing there. Static props are left flat on\n"
		"purpose (PN inflates hard-surface geometry). Off = vanilla.\n"
		"(Fine-tuning: Min Triangle Size lives in the Debugging tab.)" ),
	CVarOption( "Parallax Mapping", "r_parallax", parallaxOptions, IM_ARRAYSIZE( parallaxOptions ),
		"Per-pixel surface relief on world walls, floors and panels: a height field is\n"
		"derived from each material's normal map and marched to fake real depth. Opaque\n"
		"world (BSP) geometry only - props and characters are left to Tessellation, where\n"
		"parallax would deform their curved surfaces. Takes effect on the next map load.\n"
		"Off = vanilla." ),
	CVarOption( "Post-Processing", nullptr, postProcessOptions, IM_ARRAYSIZE( postProcessOptions ) ),
	CVarOption( "Particles", nullptr, particleOptions, IM_ARRAYSIZE( particleOptions ) ),
};

idList<VidMode> vidModes;

static bool initialFullscreen = false;
static bool initialFullscreenDesktop = false;
static int initialMode = 0;
static int initialCustomVidRes[2];
static int initialMSAAmode = 0;
static int initialUsePrecomprTextures = 0;
static int initialUseCompression = 0;
static int initialUseNormalCompr = 0;

static void CaptureGraphicsBaseline()
{
	const int curMode = r_mode.GetInteger();
	initialMode = curMode;
	int w, h;
	if ( !R_GetModeInfo( &w, &h, curMode ) ) {
		// invalid mode
		initialMode = (curMode < 0) ? -1 : 4; // safe default (800x600)
		r_mode.SetInteger( initialMode );
	}

	initialCustomVidRes[0] = r_customWidth.GetInteger();
	initialCustomVidRes[1] = r_customHeight.GetInteger();

	initialFullscreen = r_fullscreen.GetBool();
	initialFullscreenDesktop = r_fullscreenDesktop.GetBool();

	initialMSAAmode = r_multiSamples.GetInteger();

	initialUsePrecomprTextures = globalImages->image_usePrecompressedTextures.GetInteger();
	initialUseCompression = globalImages->image_useCompression.GetInteger();
	initialUseNormalCompr = globalImages->image_useNormalCompression.GetInteger();
}

static bool GraphicsHasResettableChanges()
{
	const int curMode = r_mode.GetInteger();
	if ( curMode != initialMode )
		return true;
	if ( curMode == -1 && ( initialCustomVidRes[0] != r_customWidth.GetInteger()
	                     || initialCustomVidRes[1] != r_customHeight.GetInteger() ) )
	{
		return true;
	}
	if ( r_fullscreen.GetBool() != initialFullscreen
	     || r_fullscreenDesktop.GetBool() != initialFullscreenDesktop )
	{
		return true;
	}
	if ( initialMSAAmode != r_multiSamples.GetInteger() ) {
		return true;
	}
	if ( initialUsePrecomprTextures != globalImages->image_usePrecompressedTextures.GetInteger() ) {
		return true;
	}
	if( initialUseCompression != globalImages->image_useCompression.GetInteger() ) {
		return true;
	}
	if ( initialUseNormalCompr != globalImages->image_useNormalCompression.GetInteger() ) {
		return true;
	}

	return false;
}

static bool GraphicsHasApplyableChanges()
{
	glimpParms_t curState = GLimp_GetCurState();
	int wantedWidth = 0, wantedHeight = 0;
	R_GetModeInfo( &wantedWidth, &wantedHeight, r_mode.GetInteger() );
	if ( wantedWidth != curState.width || wantedHeight != curState.height ) {
		return true;
	}
	if ( r_fullscreen.GetBool() != curState.fullScreen
	    || (curState.fullScreen && r_fullscreenDesktop.GetBool() != curState.fullScreenDesktop) )
	{
		return true;
	}
	if ( r_multiSamples.GetInteger() != curState.multiSamples ) {
		return true;
	}

	if ( initialUsePrecomprTextures != globalImages->image_usePrecompressedTextures.GetInteger() ) {
		return true;
	}
	// Note: value of image_useNormalCompression is only relevant if image_usePrecompressedTextures is enabled
	if ( initialUsePrecomprTextures
	    && (initialUseNormalCompr != globalImages->image_useNormalCompression.GetInteger()
	       || initialUseCompression != globalImages->image_useCompression.GetInteger()) )
	{
		return true;
	}

	return false;
}


static void ApplyGraphicsSettings()
{
	const char* cmd = "vid_restart partial\n";
	if ( initialUsePrecomprTextures != globalImages->image_usePrecompressedTextures.GetInteger()
	    || initialUseNormalCompr != globalImages->image_useNormalCompression.GetInteger()
	    || initialUseCompression != globalImages->image_useCompression.GetInteger() )
	{
		// these need a full restart (=> textures must be reloaded)
		cmd = "vid_restart\n";
	}
	cmdSystem->BufferCommandText( CMD_EXEC_APPEND, cmd );
}

static void GraphicsResetChanges()
{
	r_mode.SetInteger( initialMode );
	r_customWidth.SetInteger( initialCustomVidRes[0] );
	r_customHeight.SetInteger( initialCustomVidRes[1] );

	r_fullscreen.SetBool( initialFullscreen );
	r_fullscreenDesktop.SetBool( initialFullscreenDesktop );

	r_multiSamples.SetInteger( initialMSAAmode );
	globalImages->image_usePrecompressedTextures.SetInteger( initialUsePrecomprTextures );
	globalImages->image_useCompression.SetInteger( initialUseCompression );
	globalImages->image_useNormalCompression.SetInteger( initialUseNormalCompr );
}

static void InitGraphicsMenu()
{
	InitOptions( screenshotOptions, IM_ARRAYSIZE(screenshotOptions) );
	InitOptions( displayColorOptions, IM_ARRAYSIZE(displayColorOptions) );
	InitOptions( anisoOptions, IM_ARRAYSIZE(anisoOptions) );
	InitOptions( guiLayoutOptions, IM_ARRAYSIZE(guiLayoutOptions) );
	InitOptions( enhancementOptions, IM_ARRAYSIZE(enhancementOptions) );

	vidModes.SetNum(0, false);

	// modes 0-2 are not shown in the menu, probably too small
	for( int m=3; ; ++m ) {
		int w=0, h=0;
		if( ! R_GetModeInfo(&w, &h, m) )
			break;
		vidModes.Append( VidMode(w, h, m) );
	}

	std::sort( vidModes.begin(), vidModes.end(), [](const VidMode& v1, const VidMode& v2) -> bool {
		return v1.width < v2.width || (v1.width == v2.width && v1.height < v2.height);
	} );

	vidModes.Insert( VidMode(0, 0, -1), 0 );

	for ( VidMode& vm : vidModes ) {
		vm.Init();
	}

	CaptureGraphicsBaseline();
}

// Renderer backend selector. Lives at the top of the Graphics tab: it works
// on every backend and is how you switch backends to unlock the enhancement settings below.
static void DrawRendererBackend()
{
	// Renderer backend selection (DUDE). Changing this only takes effect after a
	// renderer restart. "opengl" is the legacy, vanilla-faithful ARB2 path; "opengl3"
	// is the GL 3.3 core backend and "vulkan" is the Vulkan backend — both route through
	// the RHI and enable the Graphics tab (Vulkan additionally unlocks Vulkan-only
	// features like GPU tessellation).
	ImGui::SeparatorText( "Renderer Backend" );
	{
		const char* curAPI = r_graphicsAPI.GetString();
		int backendSel = 0; // 0 = legacy opengl, 1 = opengl3, 2 = vulkan
		if ( idStr::Icmp( curAPI, "opengl3" ) == 0 ) {
			backendSel = 1;
		} else if ( idStr::Icmp( curAPI, "vulkan" ) == 0 || idStr::Icmp( curAPI, "vulkan-rt" ) == 0 ) {
			backendSel = 2;
		}
		const int oldSel = backendSel;

		ImGui::RadioButton( "Legacy (OpenGL / ARB2)", &backendSel, 0 );
		AddTooltip( "Faithful to vanilla Doom 3. Graphical enhancements are disabled." );
		ImGui::SameLine();
		ImGui::RadioButton( "OpenGL 3.3 Core", &backendSel, 1 );
		AddTooltip( "Modern GL 3.3 core backend. Enables the graphical enhancements." );
		ImGui::SameLine();
#ifdef DHEWM3_VULKAN
		ImGui::RadioButton( "Vulkan", &backendSel, 2 );
		AddTooltip( "Vulkan backend. Enables the graphical enhancements plus Vulkan-only features (GPU tessellation)." );
#else
		// this build was compiled without the Vulkan backend
		ImGui::BeginDisabled();
		ImGui::RadioButton( "Vulkan (not built)", &backendSel, 2 );
		ImGui::EndDisabled();
		AddTooltip( "This build was compiled without the Vulkan backend (-DDHEWM3_VULKAN=OFF)." );
#endif

		if ( backendSel != oldSel ) {
			const char* apiName = ( backendSel == 2 ) ? "vulkan"
			                    : ( backendSel == 1 ) ? "opengl3" : "opengl";
			r_graphicsAPI.SetString( apiName );
		}

		// If the selected backend differs from the one actually running, offer to apply
		// it. Running backend from glConfig: rhiBackend is true for GL3+Vulkan, coreProfile
		// is GL3-only (Vulkan keeps rhiBackend true with coreProfile false — RenderSystem.h).
		const int runningSel = !glConfig.rhiBackend ? 0 : ( glConfig.coreProfile ? 1 : 2 );
		if ( backendSel != runningSel ) {
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ),
				"Backend change pending - restart the renderer to apply it." );
			if ( ImGui::Button( "Apply Backend (restart renderer)" ) ) {
				cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
			}
			AddTooltip( "Runs 'vid_restart' to recreate the renderer with the selected backend." );
			if ( backendSel == 2 || runningSel == 2 ) {
				ImGui::TextDisabled( "Switching to/from Vulkan: if the UI or cursor vanishes after Apply, fully "
					"quit and relaunch (the setting is saved)." );
			}
		}
	}
}

// Live GPU / OpenGL info. Shown just below the Renderer
// Backend selector at the top of the Graphics tab.
static void DrawOpenGLInfo()
{
	if ( ImGui::TreeNode("OpenGL Info") ) {
		ImGui::BeginDisabled();
		ImGui::Text( "OpenGL vendor: %s", glConfig.vendor_string );
		ImGui::Text( "OpenGL renderer: %s", glConfig.renderer_string );
		ImGui::Text( "OpenGL version: %s", glConfig.version_string );
		if ( glConfig.glDebugOutputAvailable && glConfig.haveDebugContext ) {
			ImGui::Text( "    using an OpenGL debug context to show warnings from the OpenGL driver" );
		}
		ImGui::EndDisabled();
		ImGui::TreePop();
	} else {
		AddTooltip( "Click to show information about the currently used Graphics Card (GPU)" );
	}
}

// Display / windowing settings for the Graphics tab.
// Order matches the request: Resolution, Window Mode, Frame Rate Cap, Fullscreen Desktop,
// Vertical Sync. Resolution / Window Mode / Fullscreen Desktop need the Apply restart drawn
// by the caller; Frame Rate Cap and Vertical Sync take effect live.
static void DrawDisplaySettings()
{
	// Resolution
	static int selModeIdx = -1; // index within our vidModes array
	static int selMode = -3;    // global mode number (as used in r_mode)
	int curMode = r_mode.GetInteger();
	if ( selMode != curMode ) {
		selMode = curMode;
		int w, h;
		if ( !R_GetModeInfo( &w, &h, curMode ) ) {
			selMode = (curMode < 0) ? -1 : 4; // safe default (800x600)
			r_mode.SetInteger( selMode );
		}
		selModeIdx = 0;
		for ( int i=0, n=vidModes.Num(); i < n; ++i ) {
			if ( vidModes[i].mode == selMode ) { selModeIdx = i; break; }
		}
	}

	// Current resolution info, shown above the dropdown.
	if ( (int)glConfig.winWidth != glConfig.vidWidth ) {
		ImGui::TextDisabled( "Current Resolution: %g x %g (Physical: %d x %d)",
		                     glConfig.winWidth, glConfig.winHeight, glConfig.vidWidth, glConfig.vidHeight );
		AddDescrTooltip( "Apparently your system is using a HighDPI mode, where the logical resolution (used to specify"
		                 " window sizes) is lower than the physical resolution (number of pixels actually rendered)." );
	} else {
		ImGui::TextDisabled( "Current Resolution: %d x %d", glConfig.vidWidth, glConfig.vidHeight );
	}

	const char* resLabel = ( r_fullscreen.GetBool() && r_fullscreenDesktop.GetBool() )
		? "Window Resolution (ignored for Fullscreen Desktop Mode!)###resMode" : "Resolution###resMode";
	if ( ImGui::Combo( resLabel, &selModeIdx, [](void* data, int idx) -> const char* {
				const idList<VidMode>& vms = *static_cast< const idList<VidMode>* >(data);
				return vms[idx].label.c_str();
			}, &vidModes, vidModes.Num() ) )
	{
		selMode = vidModes[selModeIdx].mode;
		r_mode.SetInteger( selMode );
	}
	AddTooltip( "r_mode" );
	if ( selMode == -1 ) {
		int vidRes[2] = { r_customWidth.GetInteger(), r_customHeight.GetInteger() };
		if ( ImGui::InputInt2( "Custom Resolution (width x height)", vidRes ) ) {
			vidRes[0] = idMath::ClampInt( 1, 128000, vidRes[0] );
			vidRes[1] = idMath::ClampInt( 1, 128000, vidRes[1] );
			r_customWidth.SetInteger( vidRes[0] );
			r_customHeight.SetInteger( vidRes[1] );
		}
		AddTooltip( "r_customWidth / r_customHeight" );
	}

	// Window Mode
	int fullscreenChoice = r_fullscreen.GetBool();
	if ( ImGui::Combo( "Window Mode", &fullscreenChoice, "Windowed\0Fullscreen\0" ) ) {
		r_fullscreen.SetBool( fullscreenChoice != 0 );
	}
	AddTooltip( "r_fullscreen" );

	// Frame Rate Cap (com_maxFPS). Read live by the frame loop, so it applies immediately.
	int maxFps = com_maxFPS.GetInteger();
	if ( ImGui::SliderInt( "Frame Rate Cap", &maxFps, 0, 360,
	                       maxFps <= 0 ? "Uncapped" : "%d FPS", ImGuiSliderFlags_AlwaysClamp ) ) {
		com_maxFPS.SetInteger( idMath::ClampInt( 0, 1000, maxFps ) );
	}
	AddCVarOptionTooltips( com_maxFPS, "0 = uncapped. Paces frames evenly without relying on VSync. "
	                       "Pair it with VSync disabled — especially on a multi-monitor setup where VSync "
	                       "locks to the wrong refresh rate. Set it to your monitor's refresh, or a clean "
	                       "divisor of it (e.g. 72 on a 144 Hz panel)." );

	// Fullscreen Desktop
	bool fullscreenDesktop = r_fullscreenDesktop.GetBool();
	if ( ImGui::Checkbox( "Fullscreen Desktop (borderless at desktop resolution)", &fullscreenDesktop ) ) {
		r_fullscreenDesktop.SetBool( fullscreenDesktop );
	}
	AddTooltip( "r_fullscreenDesktop" );
	AddDescrTooltip( "Borderless window at the desktop resolution instead of a real mode switch; avoids desktop icons being rearranged." );

	// Make window resizable — only meaningful in windowed mode, so grey it out in fullscreen.
	ImGui::BeginDisabled( r_fullscreen.GetBool() );
	bool windowResizable = r_windowResizable.GetBool();
	if ( ImGui::Checkbox( "Make DUDE window resizable", &windowResizable ) ) {
		r_windowResizable.SetBool( windowResizable );
	}
	AddCVarOptionTooltips( r_windowResizable );
	ImGui::EndDisabled();

	// Vertical Sync (live; applied immediately via GLimp_SetSwapInterval)
	int curVsync = idMath::ClampInt( -1, 1, r_swapInterval.GetInteger() );
	if ( curVsync == -1 ) {
		curVsync = 2;
	}
	if ( ImGui::Combo( "Vertical Sync", &curVsync, "Disable VSync\0Enable VSync\0Adaptive VSync\0" ) ) {
		if ( curVsync == 2 ) {
			curVsync = -1;
		}
		if ( GLimp_SetSwapInterval( curVsync ) ) {
			r_swapInterval.SetInteger( curVsync );
			// this was just set with GLimp_SetSwapInterval(), no reason to set it again in R_CheckCvars()
			r_swapInterval.ClearModified();
		} else {
			D3::ImGuiHooks::ShowWarningOverlay( "Setting VSync (GL SwapInterval) failed, maybe try another mode" );
		}
	} else {
		AddTooltip( "r_swapInterval" );
	}
	AddDescrTooltip( "Note: Not all GPUs/drivers support Adaptive VSync" );
}

// Texture memory/quality trade-offs. Kept rather than
// pinned to max quality because texture-heavy mods / hi-res retexture packs benefit from
// compression to fit VRAM. All of these need a renderer restart (the shared Apply below).
static void DrawTextureOptions()
{
	int usePreComprTex = globalImages->image_usePrecompressedTextures.GetInteger();
	if ( ImGui::Combo( "Use precompressed (.dds) textures", &usePreComprTex,
	                   "No, only uncompressed\0Yes, no matter which format\0Only if high quality (BPCT/BC7)\0" ) )
	{
		globalImages->image_usePrecompressedTextures.SetInteger(usePreComprTex);
		// by default I guess people also want compressed normal maps when using this
		// (retexturing packs that only ship BC7 DDS files, otherwise lowres TGA normalmaps are used)
		if ( usePreComprTex ) {
			cvarSystem->SetCVarInteger( "image_useNormalCompression", 2 );
		}
	}
	const char* descr = "Use precompressed (.dds) textures. Faster loading, use less VRAM, possibly worse image quality.\n"
			"May also be used by highres retexturing packs for BC7-compressed textures (there image quality is not noticeably impaired)";
	AddCVarOptionTooltips( globalImages->image_usePrecompressedTextures, descr );

	int useCompression = globalImages->image_useCompression.GetInteger();
	if ( ImGui::Combo( "Compress uncompressed textures on load", &useCompression,
	                   "Leave uncompressed (best quality)\0Compress with S3TC (aka DXT aka BC1-3)\0Compress with BPCT (BC7)\0" ) )
	{
		globalImages->image_useCompression.SetInteger(useCompression);
	}
	descr = "When loading non-precompressed textures, compress them so they use less VRAM.\n"
			"Uncompressed has best quality. BC7 has better quality than S3TC, but may increase loading times";
	AddCVarOptionTooltips( globalImages->image_useCompression, descr );

	ImGui::BeginDisabled( !usePreComprTex && !useCompression );
	bool useNormalCompr = globalImages->image_useNormalCompression.GetBool();
	if ( ImGui::Checkbox( "Use compressed normalmaps", &useNormalCompr ) ) {
		// image_useNormalCompression 1 is not supported by modern GPUs
		globalImages->image_useNormalCompression.SetInteger(useNormalCompr ? 2 : 0);
	}
	if ( usePreComprTex ) {
		const char* descr = "Also use precompressed textures for normalmaps or compress them on load.\n"
		                    "Uncompressed often has better quality, but uses more VRAM.\n"
		                    "When using highres retexturing packs, you should definitely enable this.";
		AddCVarOptionTooltips( globalImages->image_useNormalCompression, descr );
	} else {
		AddTooltip( "Can only be used if (pre)compressed textures are enabled!" );
	}
	ImGui::EndDisabled();
}

// ---------------------------------------------------------------------------
// Enhancement quality presets (DUDE Phase 3.5)
//
// One-click tiers that set the whole GL3 enhancement suite as a group instead of
// dialling every slider by hand. This is distinct from the old vanilla image-quality
// preset (com_machineSpec, no longer user-selectable - pinned to Ultra): it only
// touches the non-vanilla enhancement cvars (SSAO, shadow maps, emissive fill,
// soft particles, post-FX).
//
// Anchor: "High" reproduces the shipped tuned defaults (calibrated on a
// GTX-1070-class card, ~12 ms/frame in a busy scene). Tiers below scale the cost
// down for weaker GL 3.3 hardware; tiers above push modern GPUs. "Potato" is the
// faithful floor - every enhancement off, which is both the cheapest and an exact
// vanilla look - so a source-accurate frame is always one click away.
//
// Presets move the *performance* levers only (on/off, resolution, sample counts,
// budgets). The artistic-calibration cvars the user tuned (SSAO intensity/floor,
// emissive reach/tint, shadow biases) are left untouched so every tier shares one
// look, just at different cost. The one exception is SSAO radius, which is
// tier-scaled (32 world units at Medium up to 128 at Nightmare) — the
// occlusion reach is treated as a cost/scope lever, not a fixed look.
enum {
	PRESET_POTATO = 0,
	PRESET_LOW,
	PRESET_MEDIUM,
	PRESET_HIGH,
	PRESET_ULTRA,
	PRESET_NIGHTMARE,
	PRESET_COUNT
};

struct EnhancementPreset {
	const char* name;
	// master toggles
	bool  softParticles;
	bool  smokeDarkBlend;
	bool  emissiveSurfaces;
	bool  ssao;
	bool  shadowMapping;
	// SSAO cost levers
	float ssaoResScale;
	int   ssaoSlices;
	int   ssaoSteps;
	bool  ssaoNormalBuffer;
	bool  ssaoBentNormal;
	// shadow-map cost levers
	int   shadowMapSize;      // 2D / spot-projected map
	int   shadowMapPointSize; // point-light cube, per face
	int   shadowMapCubePcf;
	int   shadowMapPointLimit;
	// misc
	int   emissiveLightLimit;
	float filmGrain;
	float chromaticAberration;
	float reflectionScale;    // 1.0 vanilla / 0.7 dampened for the brighter enhanced scene
	int   shading;            // r_shading: 0 vanilla LUT (Potato only) / 1 Blinn-Phong (enhanced tiers).
	                          // Dormant (still applied, just not read) on the tiers whose pbr flag
	                          // below supersedes the specular model (High and up).
	float specularScale;      // r_specularScale: scales the specular contribution (applies to all models)
	float specularExp;        // r_specularExp: Blinn-Phong/Phong exponent (ignored by the vanilla LUT)
	// shadow-map size scaling (logically part of the shadow levers above; kept here
	// so the table's positional initializers stay append-only). Inert on Potato/Low,
	// which use stencil shadows, but carried for determinism.
	bool  shadowMapSizeScale;       // r_shadowMapSizeScale: scale each light's res with its radius
	float shadowMapSizeScaleRadius; // r_shadowMapSizeScaleRadius: pivot radius that gets the base res.
	                                // A HIGHER pivot pushes more lights down a resolution tier (cheaper,
	                                // slightly softer). Ultra/Nightmare use 480 (vs 380 on Medium/High) on
	                                // purpose, NOT a typo: the trim only bites on their 2048 base, while the
	                                // lower tiers' small bases (512/1200) already floor big lights at 1/2x
	                                // regardless of the pivot, so raising it there would be a no-op.
	// baked ambient-occlusion maps (r_occlusionMaps). Appended (see note above) to keep the
	// table's positional initializers stable. On for every tier except Potato; inert on stock
	// assets and on the legacy backend, so it only shows where baked maps exist (chars, props).
	bool  occlusionMaps;            // r_occlusionMaps
	// rendering-pipeline tiers (appended, see note above): HDR scene buffer from Medium
	// up, PBR materials from High up, screen-space reflections from Ultra up. SSR marches
	// at reduced resolution (docs/ssr.md — the material weighting stays full-res): 1/2 on
	// Ultra, 2/3 on Nightmare. ssrResScale carried as 1.0 on the lower tiers so a
	// hand-enabled SSR there runs at the (full-res) default.
	bool  hdr;                      // r_hdr
	bool  pbr;                      // r_pbr
	bool  ssr;                      // r_ssr
	float ssrResScale;              // r_ssrResScale
	// filmic grain cell size (appended, see note above). Carried as the 1.5
	// default on every tier — inert on Potato, whose grain intensity is 0.
	float filmGrainSize;            // r_postFilmGrainSize
	// post-resolve antialiasing (appended, see note above): SMAA on every tier
	// above Potato. FXAA (1) is reachable by hand only — its niche is the
	// subpixel shimmer damping SMAA doesn't do, moot on the PBR tiers.
	int   rhiAA;                    // r_rhiAA: 0 = off, 1 = FXAA, 2 = SMAA
	// SSAO world-space sampling radius (appended, see note above). Fixed at 48 world
	// units across every tier (user-calibrated) — the contact-crease scale that looks
	// right regardless of preset; only the slice/step budget changes with the tier.
	float ssaoRadius;               // r_ssaoRadius: 48 on every tier
	// SSAO temporal accumulation (appended, see note above). On for Medium only:
	// it denoises the low slice/step march so Medium's cheap AO looks clean, a
	// small frame-time win over brute-forcing samples. Redundant on the higher
	// tiers (their per-frame AO is already clean) and inert on Potato/Low (SSAO
	// off), so it stays off everywhere else.
	bool  ssaoTemporal;             // r_ssaoTemporal
	// DUDE GPU tessellation (appended, see note above; Vulkan-only, inert elsewhere):
	// PN-triangle character smoothing from High up, normal-map displacement from Ultra
	// up. tessDisplace carried as 0 on the lower tiers so tessellation there (if hand-
	// enabled) is pure silhouette smoothing.
	bool  tessellation;             // r_tessellation
	float tessDisplace;             // r_tessDisplace
	// DUDE parallax occlusion mapping (appended, see note above; GL3 + Vulkan): world-surface
	// relief from Medium up. Self-shadowing (the second, half-step march) is the cost swing, so
	// it's off on Medium and full from High up. parallaxShadow is inert where parallax is off.
	bool  parallax;                 // r_parallax
	float parallaxShadow;           // r_parallaxShadow
	// DUDE GPU MD5 skinning (appended, see note above; Vulkan-only, inert on GL3). Option-B TBN is a
	// small fidelity divergence from stock's per-frame re-derive, so it is OFF on every preset for now
	// — Potato/Low stay a faithful id-render, and the higher tiers only flip it on once it is a proven
	// perf win (Milestone C strips the redundant CPU skin). Every preset forcing it 0 keeps the faithful
	// floor bulletproof even if it was hand-enabled before switching presets.
	bool  gpuSkin;                  // r_gpuSkinning
	// SSAO depth-mip acceleration (appended, see note above). On for every tier that runs SSAO
	// (Medium/High/Ultra/Nightmare) — the ~25% AO speedup for near-invisible halos at the
	// archived bias 0.1 / cap 2 defaults. Inert on Potato/Low (SSAO off). The bias/cap knobs
	// stay at their archived cvar defaults across tiers.
	bool  ssaoDepthMip;             // r_ssaoDepthMip
};

// Potato/Low keep the enhancements off but carry the cheap Medium sub-params, so
// flipping a feature on by hand from those tiers stays affordable and detection
// stays unambiguous. High == shipped defaults plus the pipeline tiers (HDR + PBR;
// see anchor note above — the pipeline columns deliberately exceed the cvar
// defaults, which keep every enhancement off).
static const EnhancementPreset enhancementPresets[PRESET_COUNT] = {
	//                soft   smoke  emiss  ssao   shadow  aoRes aoSl aoSt aoNB   aoBN   smSz  smPt  pcf ptLim emLim grain  chrom  refl  shd sScl  sExp   szScl szRad   occl   hdr    pbr    ssr    ssrRes  grainSz aa aoRad   aoTmp   tess   tessDsp  parlx  parlxSh gpuSkn dMip
	{ "Potato",       false, false, false, false, false,  0.5f, 3,   1,   false, true,  512,  512,  5,  16,   16,   0.0f,  0.0f,  1.0f, 0,  1.0f, 62.0f, true, 380.0f, false, false, false, false, 1.0f,   1.5f,   0, 48.0f,  false,  false, 0.0f, false, 0.0f, false, false },
	{ "Low",          true,  false, false, false, false,  0.5f, 3,   1,   false, true,  512,  512,  5,  16,   16,   0.05f, 0.0f,  1.0f, 1,  1.2f, 42.0f, true, 380.0f, true,  false, false, false, 1.0f,   1.5f,   2, 48.0f,  false,  false, 0.0f, false, 0.0f, false, false },
	{ "Medium",       true,  false, true,  true,  true,   0.5f, 2,   4,   false, true,  512,  512,  5,  16,   16,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 380.0f, true,  true,  false, false, 1.0f,   1.5f,   2, 48.0f,  true,   false, 0.0f, true, 0.0f, false, true },
	{ "High",         true,  false, true,  true,  true,   0.667f, 3,   6,   true,  true,  1024, 1200, 6,  64,   24,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 380.0f, true,  true,  true,  false, 1.0f,   1.5f,   2, 48.0f,  false,  true,  0.0f, true, 1.0f, false, true },
	{ "Ultra",        true,  true,  true,  true,  true,   0.75f, 4,   8,   true,  true,  2048, 2048, 8,  96,   32,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 480.0f, true,  true,  true,  true,  0.5f,   1.5f,   2, 48.0f,  false,  true,  -0.25f, true, 1.0f, false, true },
	{ "Nightmare", true, true, true, true,  true,   0.8f, 5,   10,  true,  true,  2048, 2048, 10, 128,  48,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 480.0f, true,  true,  true,  true,  0.667f, 1.5f,   2, 48.0f, false,  true,  -0.25f, true, 1.0f, false, true },
};

static void ApplyEnhancementPreset( int idx )
{
	if ( idx < 0 || idx >= PRESET_COUNT ) {
		return;
	}
	const EnhancementPreset& p = enhancementPresets[idx];

	r_useSoftParticles.SetBool( p.softParticles );
	// soft particles and smoke-darkness blend both need the depth-capture pass;
	// mirror the per-checkbox auto-enable so a preset can't leave them broken.
	if ( ( p.softParticles || p.smokeDarkBlend ) && r_enableDepthCapture.GetInteger() == 0 ) {
		r_enableDepthCapture.SetInteger( -1 );
	}
	r_smokeDarkBlend.SetBool( p.smokeDarkBlend );
	r_emissiveSurfaces.SetBool( p.emissiveSurfaces );
	r_ssao.SetBool( p.ssao );
	r_shadowMapping.SetBool( p.shadowMapping );

	r_ssaoResScale.SetFloat( p.ssaoResScale );
	r_ssaoSlices.SetInteger( p.ssaoSlices );
	r_ssaoSteps.SetInteger( p.ssaoSteps );
	r_ssaoNormalBuffer.SetBool( p.ssaoNormalBuffer );
	r_ssaoBentNormal.SetBool( p.ssaoBentNormal );
	r_ssaoRadius.SetFloat( p.ssaoRadius );
	r_ssaoTemporal.SetBool( p.ssaoTemporal );
	r_ssaoDepthMip.SetBool( p.ssaoDepthMip );

	r_shadowMapSize.SetInteger( p.shadowMapSize );
	r_shadowMapPointSize.SetInteger( p.shadowMapPointSize );
	r_shadowMapCubePcf.SetInteger( p.shadowMapCubePcf );
	r_shadowMapPointLimit.SetInteger( p.shadowMapPointLimit );
	r_shadowMapSizeScale.SetBool( p.shadowMapSizeScale );
	r_shadowMapSizeScaleRadius.SetFloat( p.shadowMapSizeScaleRadius );
	r_occlusionMaps.SetBool( p.occlusionMaps );

	r_emissiveLightLimit.SetInteger( p.emissiveLightLimit );
	r_postFilmGrain.SetFloat( p.filmGrain );
	r_postFilmGrainSize.SetFloat( p.filmGrainSize );
	r_postChromaticAberration.SetFloat( p.chromaticAberration );
	r_rhiAA.SetInteger( p.rhiAA );
	r_gl3ReflectionScale.SetFloat( p.reflectionScale );
	// specular look: model + scale + exponent, as a group. Potato stays vanilla LUT
	// at scale 1.0; enhanced tiers use Blinn-Phong at a punchier scale/exponent.
	r_shading.SetInteger( p.shading );
	r_specularScale.SetFloat( p.specularScale );
	r_specularExp.SetFloat( p.specularExp );

	// rendering-pipeline tiers: HDR (Medium+), PBR (High+), SSR (Ultra+, at
	// reduced march resolution — 1/2 Ultra, 2/3 Nightmare)
	r_hdr.SetBool( p.hdr );
	r_pbr.SetBool( p.pbr );
	r_ssr.SetBool( p.ssr );
	r_ssrResScale.SetFloat( p.ssrResScale );

	// GPU tessellation (Vulkan-only; inert on GL3): character smoothing from High,
	// normal-map displacement from Ultra.
	r_tessellation.SetBool( p.tessellation );
	r_tessDisplace.SetFloat( p.tessDisplace );

	// parallax occlusion mapping (GL3 + Vulkan): world relief from Medium, self-shadow from High.
	r_parallax.SetBool( p.parallax );
	r_parallaxShadow.SetFloat( p.parallaxShadow );

	// GPU MD5 skinning (Vulkan-only; inert on GL3). OFF on every preset for now — option-B TBN is a
	// fidelity divergence, so this keeps Potato (and every tier) a faithful id-render until it is a
	// proven perf win and deliberately flipped on for the top tiers.
	r_gpuSkinning.SetBool( p.gpuSkin );
}

// Return the preset whose full cvar vector the live cvars currently match, or -1
// for "Custom" (any hand-tweak since a preset was last applied). Every row is a
// unique vector, so a match is unambiguous.
static int DetectEnhancementPreset()
{
	for ( int i = 0; i < PRESET_COUNT; i++ ) {
		const EnhancementPreset& p = enhancementPresets[i];
		const bool match =
			r_useSoftParticles.GetBool()       == p.softParticles &&
			r_smokeDarkBlend.GetBool()         == p.smokeDarkBlend &&
			r_emissiveSurfaces.GetBool()       == p.emissiveSurfaces &&
			r_ssao.GetBool()                   == p.ssao &&
			r_shadowMapping.GetBool()          == p.shadowMapping &&
			idMath::Fabs( r_ssaoResScale.GetFloat() - p.ssaoResScale ) < 0.01f &&
			r_ssaoSlices.GetInteger()          == p.ssaoSlices &&
			r_ssaoSteps.GetInteger()           == p.ssaoSteps &&
			r_ssaoNormalBuffer.GetBool()       == p.ssaoNormalBuffer &&
			r_ssaoBentNormal.GetBool()         == p.ssaoBentNormal &&
			idMath::Fabs( r_ssaoRadius.GetFloat() - p.ssaoRadius ) < 0.5f &&
			r_ssaoTemporal.GetBool()           == p.ssaoTemporal &&
			r_ssaoDepthMip.GetBool()           == p.ssaoDepthMip &&
			r_shadowMapSize.GetInteger()       == p.shadowMapSize &&
			r_shadowMapPointSize.GetInteger()  == p.shadowMapPointSize &&
			r_shadowMapCubePcf.GetInteger()    == p.shadowMapCubePcf &&
			r_shadowMapPointLimit.GetInteger() == p.shadowMapPointLimit &&
			r_shadowMapSizeScale.GetBool()     == p.shadowMapSizeScale &&
			idMath::Fabs( r_shadowMapSizeScaleRadius.GetFloat() - p.shadowMapSizeScaleRadius ) < 0.5f &&
			r_occlusionMaps.GetBool()          == p.occlusionMaps &&
			r_emissiveLightLimit.GetInteger()  == p.emissiveLightLimit &&
			idMath::Fabs( r_postFilmGrain.GetFloat() - p.filmGrain ) < 0.005f &&
			idMath::Fabs( r_postFilmGrainSize.GetFloat() - p.filmGrainSize ) < 0.005f &&
			idMath::Fabs( r_postChromaticAberration.GetFloat() - p.chromaticAberration ) < 0.005f &&
			r_rhiAA.GetInteger()               == p.rhiAA &&
			idMath::Fabs( r_gl3ReflectionScale.GetFloat() - p.reflectionScale ) < 0.01f &&
			r_shading.GetInteger()             == p.shading &&
			idMath::Fabs( r_specularScale.GetFloat() - p.specularScale ) < 0.01f &&
			idMath::Fabs( r_specularExp.GetFloat() - p.specularExp ) < 0.5f &&
			r_hdr.GetBool()                    == p.hdr &&
			r_pbr.GetBool()                    == p.pbr &&
			r_ssr.GetBool()                    == p.ssr &&
			idMath::Fabs( r_ssrResScale.GetFloat() - p.ssrResScale ) < 0.01f &&
			r_tessellation.GetBool()           == p.tessellation &&
			idMath::Fabs( r_tessDisplace.GetFloat() - p.tessDisplace ) < 0.01f &&
			r_parallax.GetBool()               == p.parallax &&
			idMath::Fabs( r_parallaxShadow.GetFloat() - p.parallaxShadow ) < 0.01f &&
			r_gpuSkinning.GetBool()            == p.gpuSkin;
		if ( match ) {
			return i;
		}
	}
	return -1;
}

// --- Enhancements groups. Each collapsible "burger" section lives in its own helper so
// DrawGraphicsMenu() can order them freely (just reorder the calls). The five that
// wrap a CVarOption child array hoist their master switch, where they have one, onto the
// group header. ---

static void DrawEnhGroup_Lighting()
{
	if ( BeginSettingsGroup( "Lighting" ) ) {
		DrawOptions( lightingOptions, IM_ARRAYSIZE( lightingOptions ) );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_Tessellation()
{
	if ( BeginSettingsGroup( "Tessellation (only Vulkan)", &r_tessellation,
			"GPU PN-triangle tessellation that rounds the low-poly silhouettes of animated\n"
			"characters (enemies, NPCs). Vulkan only - the OpenGL backend's GL 3.3 context has\n"
			"no tessellation stages, so this does nothing there. Static props are left flat on\n"
			"purpose (PN inflates hard-surface geometry). Off = vanilla.\n"
			"(Fine-tuning: Min Triangle Size lives in the Debugging tab.)" ) ) {
		DrawOptions( tessellationOptions, IM_ARRAYSIZE( tessellationOptions ) );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_SurfaceRelief()
{
	if ( BeginSettingsGroup( "Surface Relief (Parallax)", &r_parallax,
			"Per-pixel surface relief on world walls, floors and panels: a height field is\n"
			"derived from each material's normal map and marched to fake real depth. Opaque\n"
			"world (BSP) geometry only - props and characters are left to Tessellation, where\n"
			"parallax would deform their curved surfaces. Takes effect on the next map load.\n"
			"Off = vanilla." ) ) {
		DrawOptions( parallaxOptions, IM_ARRAYSIZE( parallaxOptions ) );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_PostProcess()
{
	if ( BeginSettingsGroup( "Post-Processing" ) ) {
		DrawOptions( postProcessOptions, IM_ARRAYSIZE( postProcessOptions ) );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_Particles()
{
	if ( BeginSettingsGroup( "Particles" ) ) {
		DrawOptions( particleOptions, IM_ARRAYSIZE( particleOptions ) );
		EndSettingsGroup();
	}
}

// Antialiasing group: hardware MSAA + post-process FXAA/SMAA under one "Antialiasing" header.
// Drawn OUTSIDE the enhancement-gated block so hardware MSAA stays usable on the legacy
// backend; the post-process part is opengl3/Vulkan-only and greys out on the legacy renderer.
static void DrawGroup_Antialiasing()
{
	if ( BeginSettingsGroup( "Antialiasing" ) ) {
		// Hardware MSAA (r_multiSamples) — works on every backend, needs a renderer restart
		// (Apply at the bottom of the tab).
		static const char* msaaLevels[] = { "No Antialiasing", "2x", "4x", "8x", "16x" };
		int msaa = r_multiSamples.GetInteger();
		int msaaModeIndex = Min( 4, ( msaa > 1 ) ? idMath::ILog2( msaa ) : 0 );
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::SliderInt( "Hardware MSAA", &msaaModeIndex, 0, 4, msaaLevels[msaaModeIndex], ImGuiSliderFlags_NoInput ) ) {
			msaa = ( msaaModeIndex > 0 ) ? ( 1 << msaaModeIndex ) : 0;
			r_multiSamples.SetInteger( msaa );
		}
		AddCVarOptionTooltips( r_multiSamples, "Hardware multisample antialiasing applied across the whole 3D "
			"scene (the entire framebuffer), which makes it by far the most GPU-expensive antialiasing option "
			"here - much heavier than the post-process FXAA/SMAA below. It smooths geometry edges only, not "
			"specular/normal-map shimmer. Needs a renderer restart to change (Apply at the bottom of the tab); "
			"not all GPUs/drivers support every mode, especially 16x." );

		// Post-process AA (FXAA/SMAA) — opengl3/Vulkan only, so it greys out on the legacy backend.
		ImGui::BeginDisabled( !R_BackendSupportsEnhancements() );
		int aa = r_rhiAA.GetInteger();
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::Combo( "Post Antialiasing", &aa, "Off\0FXAA (fast, damps shimmer)\0SMAA (sharpest edges)\0" ) ) {
			r_rhiAA.SetInteger( aa );
		}
		AddTooltip( "Post-process antialiasing over the finished 3D view, on top of (and independent "
			"from) the hardware MSAA above. FXAA is one cheap pass whose subpixel smoothing "
			"also damps the specular/normal-map shimmer MSAA can't touch, at a slight overall softening. "
			"SMAA reconstructs edges much more precisely and leaves texture detail sharp, but does not "
			"treat shimmer (with PBR materials on, Toksvig already covers most of it). HUD and menus are "
			"never affected. Non-vanilla; opengl3 only. (TAA planned - see docs/antialiasing.md.)" );

		// Strength drives FXAA's subpixel term (the part that actually chases shimmer): 0 =
		// edge-only FXAA (sharpest), higher = more subpixel smoothing at some texture softening.
		ImGui::BeginDisabled( r_rhiAA.GetInteger() != 1 );
		float fxaaStrength = r_fxaaStrength.GetFloat();
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::SliderFloat( "FXAA Strength", &fxaaStrength, 0.0f, 1.0f, "%.2f" ) ) {
			r_fxaaStrength.SetFloat( fxaaStrength );
		}
		AddTooltip( "How aggressively FXAA smooths subpixel detail. 0 antialiases edges only (sharpest, "
			"least shimmer reduction); higher values blend fine subpixel detail toward its neighbourhood, "
			"cutting more of the specular/normal-map crawl but softening textures slightly. ~0.75 is a "
			"good balance." );
		ImGui::EndDisabled();	// FXAA strength gate
		ImGui::EndDisabled();	// post-process supported gate
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_AmbientOcclusion()
{
	// Ambient Occlusion (DUDE Phase 3.5). Master toggle + the quality/perf stops; the fine
	// tuning sliders live in the Debugging tab. Read live by the renderer (no restart).
	if ( BeginSettingsGroup( "Ambient Occlusion" ) ) {
		bool ssao = r_ssao.GetBool();
		if ( ImGui::Checkbox( "SSAO (GTAO ambient occlusion)", &ssao ) ) {
			r_ssao.SetBool( ssao );
		}
		AddTooltip( "Screen-space ambient occlusion (GTAO): darkens creases, corners and contact "
			"points in the ambient light only, so models look grounded instead of flat/plastic. It "
			"never touches direct/dynamic lights, so it stays correct as lighting changes. Tuning "
			"sliders are in the Developer tab. Non-vanilla; opengl3 only." );

		// Resolution: discrete quality/perf stops from half to full screen resolution.
		ImGui::BeginDisabled( !r_ssao.GetBool() );
		const float ssaoResStops[]  = { 0.5f, 0.667f, 0.75f, 0.8f, 1.0f };
		const char *ssaoResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Four-fifths (4/5)", "Full" };
		const int ssaoNumStops = IM_ARRAYSIZE( ssaoResStops );
		const float curScale = r_ssaoResScale.GetFloat();
		int ssaoResIdx = 0;
		float ssaoResBest = 1e9f;
		for ( int i = 0; i < ssaoNumStops; i++ ) {
			const float d = idMath::Fabs( curScale - ssaoResStops[i] );
			if ( d < ssaoResBest ) { ssaoResBest = d; ssaoResIdx = i; }
		}
		if ( ImGui::SliderInt( "Resolution", &ssaoResIdx, 0, ssaoNumStops - 1,
				ssaoResLabels[ssaoResIdx], ImGuiSliderFlags_NoInput ) ) {
			ssaoResIdx = ssaoResIdx < 0 ? 0 : ( ssaoResIdx >= ssaoNumStops ? ssaoNumStops - 1 : ssaoResIdx );
			r_ssaoResScale.SetFloat( ssaoResStops[ssaoResIdx] );
		}
		AddTooltip( "Resolution the AO buffer is computed at, as a fraction of the screen. Half is "
			"~4x cheaper and a little softer; Full is sharpest and most expensive; 3/4 and 4/5 sit "
			"between. Lower this first if SSAO costs too much." );

		// Temporal accumulation: reuse the previous frame's AO (reprojected by camera
		// motion) so the horizon-search noise settles and slices/steps can run lower.
		bool ssaoTemporal = r_ssaoTemporal.GetBool();
		if ( ImGui::Checkbox( "Temporal Accumulation", &ssaoTemporal ) ) {
			r_ssaoTemporal.SetBool( ssaoTemporal );
		}
		AddTooltip( "Blend AO across frames (reprojected as the camera moves) instead of recomputing "
			"it fresh each frame. Smooths the AO and lets the Directions/Steps run lower for the same "
			"look, so it's cheaper on weaker GPUs. Ghosting on fast motion is clamped automatically; "
			"the Feedback strength lives in the Developer tab. Non-vanilla; opengl3 only." );

		// Depth-mip acceleration (docs/ssao-perf-optimization.md). A prefiltered linear-depth
		// mip chain: far horizon steps read a coarse, cache-local mip instead of scattering
		// across full-res depth. ~40% cheaper AO at a wide radius (measured r_ssaoDepthMip
		// 0 vs 1 = 74 -> 104 fps). Trade-off: a faint silhouette halo. Standalone preference,
		// not preset-driven — a preset change shouldn't override the fidelity choice.
		bool ssaoDepthMip = r_ssaoDepthMip.GetBool();
		if ( ImGui::Checkbox( "Depth-Mip Acceleration", &ssaoDepthMip ) ) {
			r_ssaoDepthMip.SetBool( ssaoDepthMip );
		}
		AddTooltip( "Speed up the AO horizon search by reading a prefiltered depth mip chain: far "
			"samples read a coarser, cache-friendly copy of the depth buffer instead of scattering "
			"across full-resolution depth. Big win at a wide radius (~40% cheaper AO here). "
			"Trade-off: a faint dark halo can appear around object silhouettes, since one coarse "
			"depth texel can't represent both surfaces at an edge. Off = the exact full-resolution "
			"depth march (most accurate, most expensive). Recommended on. Non-vanilla; opengl3/Vulkan." );
		ImGui::EndDisabled();

		// Baked occlusion maps: independent of SSAO (works with it off). Inert unless a
		// material ships an occlusionmap stage, which no stock Doom 3 asset does. Strength
		// sliders live in the Developer tab, like the SSAO tuning.
		bool oclMaps = r_occlusionMaps.GetBool();
		if ( ImGui::Checkbox( "Baked Occlusion Maps", &oclMaps ) ) {
			r_occlusionMaps.SetBool( oclMaps );
		}
		AddTooltip( "Use per-material baked ambient-occlusion textures (the `occlusionmap` material "
			"stage) to darken creases in the ambient and direct-light diffuse, the same way SSAO "
			"does but from art-authored maps. Only affects materials that ship an occlusion map "
			"(no stock Doom 3 asset does, so this is inert on the base game and aimed at mods / "
			"custom art). Complements SSAO; strength sliders are in the Developer tab. Non-vanilla; "
			"opengl3 only." );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_Reflections()
{
	// Reflections (DUDE PBR Phase C.2, docs/ssr.md). Master switch on the group header +
	// the quality/perf stops here; the tuning sliders live in the Developer tab next to
	// the PBR knobs.
	if ( BeginSettingsGroup( "Reflections", &r_ssr,
			"Glossy and metallic surfaces (polished floors, bare metal — per the PBR "
			"material table) mirror the on-screen scene: fixtures, screens, characters. "
			"Reflections are screen-space, so off-screen objects can't appear and rays fade at "
			"the screen edges. Works with PBR shading on or off; tuning sliders are in the "
			"Developer tab. Non-vanilla; opengl3 only." ) ) {
		// Resolution: discrete quality/perf stops for the reflection march buffer.
		const float ssrResStops[]  = { 0.5f, 0.667f, 0.75f, 1.0f };
		const char *ssrResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Full" };
		const int ssrNumStops = IM_ARRAYSIZE( ssrResStops );
		const float ssrCurScale = r_ssrResScale.GetFloat();
		int ssrResIdx = 0;
		float ssrResBest = 1e9f;
		for ( int i = 0; i < ssrNumStops; i++ ) {
			const float d = idMath::Fabs( ssrCurScale - ssrResStops[i] );
			if ( d < ssrResBest ) { ssrResBest = d; ssrResIdx = i; }
		}
		if ( ImGui::SliderInt( "Resolution##ssr", &ssrResIdx, 0, ssrNumStops - 1,
				ssrResLabels[ssrResIdx], ImGuiSliderFlags_NoInput ) ) {
			ssrResIdx = ssrResIdx < 0 ? 0 : ( ssrResIdx >= ssrNumStops ? ssrNumStops - 1 : ssrResIdx );
			r_ssrResScale.SetFloat( ssrResStops[ssrResIdx] );
		}
		AddTooltip( "Resolution the reflection rays are marched at, as a fraction of the screen. "
			"The material response (Fresnel, gloss) always applies at full resolution, so lower "
			"stops only soften the reflected image — Half is ~4x cheaper and pairs well with "
			"Temporal Accumulation. Lower this first if reflections cost too much." );

		// Temporal accumulation: rotate the march jitter per frame and average the
		// results (reprojected by camera motion) so the grain resolves.
		bool ssrTemporal = r_ssrTemporal.GetBool();
		if ( ImGui::Checkbox( "Temporal Accumulation##ssr", &ssrTemporal ) ) {
			r_ssrTemporal.SetBool( ssrTemporal );
		}
		AddTooltip( "Blend reflections across frames (reprojected as the camera moves) instead of "
			"recomputing them fresh each frame. The march's grainy sparkle settles into a clean "
			"image; ghosting on fast motion is clamped automatically. The Feedback strength lives "
			"in the Developer tab. Non-vanilla; opengl3 only." );

		// Hi-Z acceleration (docs/ssao-perf-optimization.md, r_ssrHiZ). A min-Z depth pyramid
		// lets the march leap provably-empty span instead of stepping it. Pure perf option —
		// reflections are pixel-identical. Marginal + scene-dependent, so default-off + opt-in;
		// leap aggressiveness is the dev cvar r_ssrHiZLevel.
		bool ssrHiZ = r_ssrHiZ.GetBool();
		if ( ImGui::Checkbox( "Hi-Z Acceleration##ssr", &ssrHiZ ) ) {
			r_ssrHiZ.SetBool( ssrHiZ );
		}
		AddTooltip( "Speed up the reflection ray-march by leaping over empty space with a depth "
			"pyramid instead of stepping through it. Reflections look identical — this is a pure "
			"performance option, no visual change. The gain is scene-dependent: a few percent on "
			"open / distant reflections, and essentially nothing on grazing reflective floors "
			"(where the rays hug the surface). Best left off on older GPUs. Non-vanilla; opengl3/Vulkan." );

		// Glass: baked room probes replace the generic env/gen* cubemap (docs/ssr.md).
		bool ssrProbes = r_ssrGlassProbes.GetBool();
		if ( ImGui::Checkbox( "Glass Reflections##ssr", &ssrProbes ) ) {
			r_ssrGlassProbes.SetBool( ssrProbes );
		}
		AddTooltip( "Glass reflects a snapshot of the actual room (captured automatically the "
			"first time you enter an area, cached to disk) instead of Doom 3's generic blurry "
			"cubemap - panes mirror the real room at any viewing angle. Re-capture an area with "
			"the bakeGlassProbe console command; brightness slider in the Developer tab. "
			"Non-vanilla; opengl3 only." );
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_Shadows()
{
	// Shadows (DUDE Phase 3.5). The master switch on the group header greys the whole
	// group out when off, making it clear the sub-settings all take effect together.
	// Every value is read live by the renderer (no restart).
	if ( BeginSettingsGroup( "Shadows", &r_shadowMapping,
			"Soft shadow maps for projected/spot and point lights instead of hard "
			"stencil shadow volumes. While on, stencil shadows are fully off: lights without "
			"a shadow map (parallel, or out-of-budget point) render unshadowed. This isolates "
			"shadow-map cost for profiling. Off = vanilla stencil shadows everywhere." ) ) {

		int res = r_shadowMapSize.GetInteger();
		if ( ImGui::SliderInt( "Spot / Projected Map Resolution", &res, 256, 4096 ) ) {
			r_shadowMapSize.SetInteger( res );
		}
		AddTooltip( "Resolution of the 2D depth map used only by projected / spot lights. "
			"Point (omni) lights are the bulk of Doom 3's shadows and use the separate Point / "
			"Cube Map Resolution slider below instead — so on maps with no shadow-casting "
			"spotlight this has no visible effect (r_shadowMapDebug 1 shows the '2D-mapped' "
			"count). Higher = sharper spot-shadow edges, more memory." );

		int pres = r_shadowMapPointSize.GetInteger();
		if ( ImGui::SliderInt( "Point / Cube Map Resolution", &pres, 128, 4096 ) ) {
			r_shadowMapPointSize.SetInteger( pres );
		}
		AddTooltip( "Per-face resolution of the point-light cube map — this is the one that "
			"affects most Doom 3 shadows. Six faces cover the whole sphere, so it can be lower "
			"than the spot/projected map for similar quality. Higher = sharper, more memory and "
			"fill (6 faces). Pair with Cube Edge Softness (Developer tab) to smooth edges without "
			"raising resolution." );

		bool perf = r_shadowMapPerforated.GetBool();
		if ( ImGui::Checkbox( "Perforated Casters (grates/fences)", &perf ) ) {
			r_shadowMapPerforated.SetBool( perf );
		}
		AddTooltip( "Let alpha-tested grates, fences and foliage cast real punched-out "
			"shadows. In vanilla these are flagged noShadows because stencil volumes can't "
			"perforate, so they cast nothing (or a hand-faked shadow). Shadow maps can cut "
			"the holes, so this enables their true shadow. Turn off to keep vanilla behaviour." );

		bool viewWeapon = r_shadowMapViewWeapon.GetBool();
		if ( ImGui::Checkbox( "View Weapon Casts Shadows", &viewWeapon ) ) {
			r_shadowMapViewWeapon.SetBool( viewWeapon );
		}
		AddTooltip( "Let the first-person weapon (and the player's arms) cast shadow-map "
			"shadows. Off (default) keeps the weapon out of the map, so it never throws a "
			"gun-shaped shadow onto nearby floors/walls (a few weapons — chainsaw chain, "
			"plasmagun canister — would otherwise cast because id didn't flag them noShadows). "
			"On lets it cast: the gun self-shadows but also shadows the world, since one "
			"shadow map can't separate the two." );

		// Point (omni) lights use a 6-face cube map — the bulk of Doom 3's shadows.
		// Budgeting how many get one keeps the cost bounded in busy rooms; the rest
		// keep their vanilla stencil shadows, so there is no fidelity loss.
		ImGui::SeparatorText( "Point Lights" );

		int limit = r_shadowMapPointLimit.GetInteger();
		if ( ImGui::SliderInt( "Point Light Budget", &limit, 0, 128 ) ) {
			r_shadowMapPointLimit.SetInteger( limit );
		}
		AddTooltip( "How many point lights get a soft cube shadow map per view, chosen by "
			"on-screen importance. While shadow mapping is on, out-of-budget point lights "
			"render unshadowed (stencil shadows are fully off). Lower = faster in crowded "
			"scenes. 0 = all point lights (default; most consistent, slowest)." );

		EndSettingsGroup();
	}
}

static void DrawGraphicsMenu()
{
	ImGui::Spacing();

	// --- Display / renderer settings (work on every backend). These stay above and
	// outside the enhancement-only block below. ---
	DrawRendererBackend();
	DrawOpenGLInfo();

	// Display / windowing / output settings — a collapsible "burger" group like the
	// enhancement groups below. Renderer Backend stays pinned above it.
	if ( BeginSettingsGroup( "Display" ) ) {
		DrawDisplaySettings();
		// Brightness / Gamma (live). These took the spot the MSAA slider used to hold; MSAA
		// now lives in the "Antialiasing" group further down.
		DrawOptions( displayColorOptions, IM_ARRAYSIZE( displayColorOptions ) );
		EndSettingsGroup();
	}

	ImGui::Spacing();
	ImGui::TextDisabled( "Enhancements that deviate from vanilla Doom 3." );
	ImGui::Spacing();

	const bool supported = R_BackendSupportsEnhancements();
	if ( !supported ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ),
			"These settings don't apply on the current (legacy) backend." );
		ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ),
			"Choose OpenGL 3.3 as the Renderer Backend at the top of this tab to enable them." );
		ImGui::Spacing();
	}

	// grey everything out (and make it non-interactive) when the running backend
	// can't use these effects - they simply don't apply then.
	ImGui::BeginDisabled( !supported );

	// One-click quality presets for the whole enhancement suite. The combo is the
	// user's target; the status line reflects the live cvars (so hand-tweaking any
	// slider below reads back as "Custom"). Nothing is applied until "Apply".
	ImGui::SeparatorText( "Quality Preset" );
	{
		static int selPreset = -1;
		static int lastDetected = -2;	// -2 = "not seen yet" -> forces a first-frame sync

		// The live enhancement cvars are the shared source of truth. Re-detect every
		// frame so an external actor snaps this combo into agreement: the classic Doom
		// quality selector (its choiceDef drives dude_preset, then applies) or a console
		// "dudePreset". Detection returning -1 means "Custom" (a slider was hand-tweaked),
		// which isn't a valid combo index - leave the combo where the user left it then.
		const int detected = DetectEnhancementPreset();
		if ( selPreset < 0 ) {
			selPreset = ( detected >= 0 ) ? detected : PRESET_HIGH;
		}
		if ( detected >= 0 && detected != lastDetected ) {
			selPreset = detected;
		}
		lastDetected = detected;

		ImGui::SetNextItemWidth( 220.0f );
		ImGui::Combo( "##enhPreset", &selPreset,
			"Potato\0Low\0Medium\0High\0Ultra\0Nightmare\0" );
		ImGui::SameLine();
		if ( ImGui::Button( "Apply Preset" ) ) {
			ApplyEnhancementPreset( selPreset );
			// keep the shared cvar honest so the classic Doom selector agrees at once;
			// mark it as ours so this frame's re-detect isn't treated as external.
			dude_preset.SetInteger( selPreset );
			lastDetected = selPreset;
		}
		AddTooltip( "One-click tiers for the whole enhancement suite (SSAO, shadow maps, emissive "
			"fill light, soft particles, post-FX, specular look) plus the rendering pipeline: HDR "
			"from Medium up, PBR materials from High up, screen-space reflections from Ultra up "
			"(at half march resolution; two-thirds on Nightmare). 'Potato' is the "
			"vanilla-faithful floor (everything off, vanilla specular) and the fastest. Presets set "
			"the shading look and the performance levers; your fine-tuning (SSAO radii, emissive "
			"reach/tint, shadow biases, PBR category sliders) is left alone. Tweaking any slider "
			"afterwards shows 'Custom'." );

		if ( detected < 0 ) {
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ), "Current: Custom (hand-tuned)" );
		} else {
			ImGui::TextDisabled( "Current: %s", enhancementPresets[detected].name );
		}
	}
	ImGui::Spacing();

	// Collapsible "burger" groups, ordered for a typical player (most visible impact
	// first). Reorder these calls to change the on-screen order.
	DrawEnhGroup_Shadows();
	DrawEnhGroup_AmbientOcclusion();
	DrawEnhGroup_SurfaceRelief();
	DrawEnhGroup_Tessellation();
	DrawEnhGroup_Particles();
	DrawEnhGroup_Lighting();
	DrawEnhGroup_Reflections();
	DrawEnhGroup_PostProcess();

	ImGui::EndDisabled();

	// Antialiasing (hardware MSAA + post-process FXAA/SMAA). Outside the enhancement gate so
	// MSAA stays usable on the legacy backend; the post-process part greys out there.
	DrawGroup_Antialiasing();

	// Anisotropic Filtering — its own small collapsible group (works on every backend, live).
	if ( BeginSettingsGroup( "Anisotropic Filtering" ) ) {
		DrawOptions( anisoOptions, IM_ARRAYSIZE( anisoOptions ) );
		EndSettingsGroup();
	}

	// Texture Options — works on every backend, so it sits at the bottom outside the
	// enhancement-only block, as its own collapsible group.
	if ( BeginSettingsGroup( "Texture Options" ) ) {
		DrawTextureOptions();
		EndSettingsGroup();
	}

	// Shared Apply / Reset for all the video settings that need a renderer restart
	// (resolution, window mode, fullscreen-desktop, MSAA, textures). Frame Rate Cap and
	// Vertical Sync take effect live and are not covered here.
	if ( !GraphicsHasApplyableChanges() ) {
		ImGui::BeginDisabled();
		ImGui::Button( "Apply" );
		AddTooltip( "No changes were made, so there's nothing to apply" );
		ImGui::EndDisabled();
	} else {
		if ( ImGui::Button( "Apply" ) ) {
			ApplyGraphicsSettings();
		}
		AddTooltip( "Click to apply the display / texture settings above; restarts the renderer (but not the game)." );
	}
	ImGui::SameLine();
	if ( !GraphicsHasResettableChanges() ) {
		ImGui::BeginDisabled();
		ImGui::Button( "Reset" );
		AddTooltip( "Nothing has changed since the menu was opened, so there's nothing to reset" );
		ImGui::EndDisabled();
	} else {
		if ( ImGui::Button( "Reset" ) ) {
			GraphicsResetChanges();
		}
		AddTooltip( "Restore the display / texture settings to what they were when this menu was opened" );
	}

	// GUI and Layout — plain section (not a burger), just above Screenshots.
	ImGui::Spacing();
	DrawOptions( guiLayoutOptions, IM_ARRAYSIZE( guiLayoutOptions ) );

	// Screenshots — live (no restart), so it sits at the very bottom.
	ImGui::Spacing();
	DrawOptions( screenshotOptions, IM_ARRAYSIZE( screenshotOptions ) );
}

// Developer tab: live render-debug toggles, shadow-map tuning and emissive-surface
// controls. Not a "faithful" tab — purely for dialling values in-game during development.
// DUDE PBR per-category defaults grid (docs/pbr-materials.md): an 8x4 table of the
// {metalness, roughness, wetness, env} preset each category drives. Edits the live
// defaults table (R_PbrSetCategoryDefault); category-tagged materials pick it up next
// frame, no reload. Returns true if any value changed. Shared by the Developer tab and
// the material editor's Categories tab. Env glow only does anything on metals; wetness
// and (on organics) metalness are available for uniformity even where near-inert.
static bool PbrCategoryGrid()
{
	static const struct { int cat; const char *label; } rows[] = {
		{ PBR_CAT_METAL,   "Bare Metal" },
		{ PBR_CAT_PAINTED, "Painted Metal" },
		{ PBR_CAT_CERAMIC, "Ceramic Sheen" },
		{ PBR_CAT_RUST,    "Rusted Metal" },
		{ PBR_CAT_STONE,   "Stone / Concrete" },
		{ PBR_CAT_SKIN,    "Skin (faces)" },
		{ PBR_CAT_EYES,    "Eyes / Teeth" },
		{ PBR_CAT_FLESH,   "Flesh / Gore" },
	};
	bool changed = false;
	const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_RowBg;
	if ( ImGui::BeginTable( "pbr_categories", 5, flags ) ) {
		ImGui::TableSetupColumn( "Category", ImGuiTableColumnFlags_WidthFixed, 130.0f );
		ImGui::TableSetupColumn( "Metalness" );
		ImGui::TableSetupColumn( "Roughness" );
		ImGui::TableSetupColumn( "Wetness" );
		ImGui::TableSetupColumn( "Env glow" );
		ImGui::TableHeadersRow();
		for ( int i = 0; i < IM_ARRAYSIZE( rows ); i++ ) {
			float m, r, w, e;
			R_PbrCategoryDefaults( rows[i].cat, m, r, w, e );
			ImGui::TableNextRow();
			ImGui::PushID( rows[i].cat );
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted( rows[i].label );
			bool c = false;
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth( -FLT_MIN ); c |= ImGui::SliderFloat( "##m", &m, 0.0f,  1.0f, "%.2f" );
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth( -FLT_MIN ); c |= ImGui::SliderFloat( "##r", &r, 0.03f, 1.0f, "%.2f" );
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth( -FLT_MIN ); c |= ImGui::SliderFloat( "##w", &w, 0.0f,  4.0f, "%.2f" );
			ImGui::TableNextColumn(); ImGui::SetNextItemWidth( -FLT_MIN ); c |= ImGui::SliderFloat( "##e", &e, 0.0f,  4.0f, "%.2f" );
			if ( c ) { R_PbrSetCategoryDefault( rows[i].cat, m, r, w, e ); changed = true; }
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	return changed;
}

// --- Debugging groups. Each collapsible section is its own helper so DrawShadowDebugMenu()
// can order them freely (just reorder the calls). The backend-gated sections (shadow maps,
// emissive, glass, SSAO, occlusion maps) grey their own body out on the legacy renderer; the
// general / PBR / SSR sections stay live on every backend. Section bodies are left at their
// original indentation to keep the diff readable. ---

static void DrawDbgGroup_RenderDebugging()
{
	// General render-debug toggles — core cvars that work on every backend (not
	// enhancement-gated). Pinned to the top and always shown (not collapsible).
	ImGui::SeparatorText( "Render Debugging" );

	int whiteWorld = r_whiteWorld.GetInteger();
	const char *whiteWorldModes[] = { "Off", "White (lighting + colour)", "Clay (occlusion only)" };
	if ( ImGui::Combo( "White World", &whiteWorld, whiteWorldModes, IM_ARRAYSIZE( whiteWorldModes ) ) ) {
		r_whiteWorld.SetInteger( whiteWorld );
	}
	AddTooltip( "Render diffuse as white to read lighting/occlusion in isolation. Clay also neutralises "
	            "light + material colour and forces metalness 0, so only the SSAO + POM occlusion/relief "
	            "shows, in grey." );

	int showTris = r_showTris.GetInteger();
	if ( ImGui::SliderInt( "Wireframe", &showTris, 0, 3 ) ) {
		r_showTris.SetInteger( showTris );
	}
	AddTooltip( "r_showTris: 0 = off, 1 = visible triangles, 2 = all front-facing, 3 = all." );

	bool lights = !r_skipInteractions.GetBool();
	if ( ImGui::Checkbox( "Lights (dynamic lighting)", &lights ) ) {
		r_skipInteractions.SetBool( !lights );
	}
	AddTooltip( "Off skips all light/surface interaction drawing (r_skipInteractions); the scene keeps "
		"only its ambient/emissive passes." );

	bool shadows = r_shadows.GetBool();
	if ( ImGui::Checkbox( "Stencil Shadows", &shadows ) ) {
		r_shadows.SetBool( shadows );
	}
	AddTooltip( "Toggles stencil shadow volumes (the vanilla technique) and self-shadowing (r_shadows). "
		"On opengl3/Vulkan with Shadow Mapping enabled, shadows come from the shadow maps below instead, "
		"so this only affects the stencil path." );

	ImGui::Spacing();
}

static void DrawDbgGroup_Tessellation()
{
	// DUDE tessellation (docs/tessellation.md): the fine-grain size threshold, seam welding
	// and the material logger. The on/off + level live in the Graphics tab (Tessellation);
	// this is tuning only. Vulkan only.
	if ( BeginSettingsGroup( "Tessellation" ) ) {
	if ( !r_tessellation.GetBool() ) {
		ImGui::TextDisabled( "Tessellation is off — enable \"Mesh Tessellation\" in the Graphics tab first." );
	}
	ImGui::BeginDisabled( !r_tessellation.GetBool() );
	float tessMinEdge = r_tessMinEdge.GetFloat();
	if ( ImGui::SliderFloat( "Tessellation Min Triangle Size", &tessMinEdge, 0.0f, 8.0f, "%.2f", 0 ) ) {
		r_tessMinEdge.SetFloat( tessMinEdge );
	}
	AddTooltip( "r_tessMinEdge: triangles with edges finer than this (world units) stay flat, so dense "
		"detail (eye-sockets, faces) doesn't over-inflate under PN while larger silhouette triangles still "
		"smooth. Raise if faces/fine features distort; lower to smooth more aggressively. Vulkan only." );

	bool tessDebug = r_tessDebug.GetBool();
	if ( ImGui::Checkbox( "Log Tessellated Materials", &tessDebug ) ) {
		r_tessDebug.SetBool( tessDebug );
	}
	AddTooltip( "r_tessDebug: print each material name accepted for tessellation once (with model type and "
		"entity index) to the console, so a mis-tessellated surface can be identified. Vulkan only." );

	bool tessWeld = r_tessWeldSeams.GetBool();
	if ( ImGui::Checkbox( "Weld Seam Normals", &tessWeld ) ) {
		r_tessWeldSeams.SetBool( tessWeld );
	}
	AddTooltip( "r_tessWeldSeams: average coincident vertex normals on animated meshes so a model built from "
		"mirrored/UV-split halves deforms as one piece (no seam opening) under tessellation + displacement. "
		"Off = vanilla normals." );

	ImGui::BeginDisabled( !r_tessWeldSeams.GetBool() );
	float tessWeldThr = r_tessWeldThreshold.GetFloat();
	if ( ImGui::SliderFloat( "Seam Weld Threshold", &tessWeldThr, 0.0f, 1.0f, "%.2f", 0 ) ) {
		r_tessWeldThreshold.SetFloat( tessWeldThr );
	}
	AddTooltip( "r_tessWeldThreshold: only coincident normals whose dot product is at least this get welded. "
		"1 = weld only identical normals; lower also welds sharper creases (0.7 closes seams while keeping "
		"hard edges)." );
	ImGui::EndDisabled();
	ImGui::EndDisabled();
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_PBR()
{
	// DUDE PBR materials (docs/pbr-materials.md): every live tuning knob for the
	// GGX interaction path in one place, so the look can be dialled in-game.
	// All values apply instantly (per-draw uniforms, no reloadShaders needed).
	if ( BeginSettingsGroup( "PBR Materials (GGX)" ) ) {

	if ( !r_pbr.GetBool() ) {
		ImGui::TextDisabled( "PBR is off — enable \"PBR Materials (GGX)\" in the Graphics tab first." );
	}
	ImGui::BeginDisabled( !r_pbr.GetBool() );

	float pbrRough = r_pbrRoughness.GetFloat();
	if ( ImGui::SliderFloat( "Fallback Roughness", &pbrRough, 0.03f, 1.0f, "%.2f" ) ) {
		r_pbrRoughness.SetFloat( pbrRough );
	}
	AddTooltip( "r_pbrRoughness: roughness for materials without a pbr-table entry (0.03 = mirror, 1 = matte). "
		"0.58 matches the Blinn-Phong exponent-16 highlight width. Per-material values come from "
		"pbr/pbr_materials.cfg and override this." );

	float pbrSpec = r_pbrSpecScale.GetFloat();
	if ( ImGui::SliderFloat( "Specular Energy (dielectrics)", &pbrSpec, 0.0f, 8.0f, "%.2f" ) ) {
		r_pbrSpecScale.SetFloat( pbrSpec );
	}
	AddTooltip( "r_pbrSpecScale: artistic boost on the GGX lobe for dielectrics/painted surfaces (fades to 1 "
		"on bare metal, whose albedo-derived reflectance is already bright). The physical 4% dielectric "
		"reflectance reads dim in Doom 3's display-space shading; this compensates." );

	float pbrToksvig = r_pbrToksvigBase.GetFloat();
	if ( ImGui::SliderFloat( "Highlight Tightness (Toksvig base)", &pbrToksvig, 0.0f, 0.6f, "%.2f" ) ) {
		r_pbrToksvigBase.SetFloat( pbrToksvig );
	}
	AddTooltip( "r_pbrToksvigBase: how much normal-map variance is ignored before it widens (softens) the "
		"highlight. LOWER = softer highlights but strong anti-firefly filtering; HIGHER = tighter, punchier "
		"highlights but white pixel spikes creep back on panel seams. 0.2 = calibrated split." );

	float pbrClamp = r_pbrFireflyClamp.GetFloat();
	if ( ImGui::SliderFloat( "Highlight Ceiling (firefly clamp)", &pbrClamp, 1.0f, 16.0f, "%.1f" ) ) {
		r_pbrFireflyClamp.SetFloat( pbrClamp );
	}
	AddTooltip( "r_pbrFireflyClamp: upper bound on the specular lobe. Stops isolated texels clipping to pure "
		"white; also the ceiling that skin/tight highlights ride at. Raise for hotter cores (pairs well "
		"with HDR), lower to flatten everything." );

	float metalDiffuse = r_pbrMetalDiffuse.GetFloat();
	if ( ImGui::SliderFloat( "Metal Color Retention", &metalDiffuse, 0.0f, 1.0f, "%.2f" ) ) {
		r_pbrMetalDiffuse.SetFloat( metalDiffuse );
	}
	AddTooltip( "r_pbrMetalDiffuse: how much of a metal's painted albedo color survives. Physical PBR kills "
		"diffuse on metals (0) and pushes the color into reflections Doom 3 can't supply, so metals read dark "
		"and off-color; raise this to keep the asset look while the metallic specular still rides on top "
		"(1 = keep all, 0 = physical). Weighted by metalness, so it mainly affects bare metal." );

	float envScale = r_pbrEnvScale.GetFloat();
	if ( ImGui::SliderFloat( "Metal Environment Glow", &envScale, 0.0f, 2.0f, "%.2f" ) ) {
		r_pbrEnvScale.SetFloat( envScale );
	}
	AddTooltip( "r_pbrEnvScale: metals reflect their surroundings, but Doom 3 has no environment probes — "
		"this stands in by letting metal reflect a tinted share of each light's own energy, so it stops "
		"going black where the highlight misses. Scales with light and shadow (metals stay dark in "
		"darkness). With this up, the Metalness Cap can rise toward 1." );

	// per-category presets: the 8x4 grid every tagged material tracks live. A pinned
	// (none) or per-material override still keeps its own values (edit those in the
	// in-game material editor). Metalness/roughness = surface response; wetness = a
	// specular-energy film; env glow = metal reflection floor (inert on non-metals).
	ImGui::Spacing();
	ImGui::TextDisabled( "Material category presets (live; a pinned/override material keeps its own):" );
	PbrCategoryGrid();
	if ( ImGui::Button( "Save Category Defaults" ) ) {
		R_PbrWriteCategoryDefaults();
	}
	AddTooltip( "Writes the 8 category rows above as @cat lines in pbr/pbr_overrides.cfg (the dude folder) "
		"so they persist across launches. Per-material pins go through Save in the in-game material editor." );

	ImGui::Spacing();
	if ( ImGui::Button( "Reload PBR Table" ) ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "reloadPbrTable\n" );
	}
	AddTooltip( "Re-reads pbr/pbr_materials.cfg and pbr/pbr_overrides.cfg and re-applies them to all loaded "
		"materials. Workflow for a single surface: r_showSurfaceInfo 1 to read its material name, add a "
		"\"<material> <metalness> <roughness>\" line to base/pbr/pbr_overrides.cfg, then press this." );

	ImGui::EndDisabled();
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_SSR()
{
	// SSR tuning (docs/ssr.md). Deliberately outside the r_pbr-disabled block: SSR
	// reads the same material table but works with PBR shading off. The on/off switch
	// lives in the Graphics tab (Reflections); this is tuning only.
	if ( BeginSettingsGroup( "Screen-Space Reflections (SSR)" ) ) {

	if ( !r_ssr.GetBool() ) {
		ImGui::TextDisabled( "SSR is off — enable \"Screen-Space Reflections\" in the Graphics tab first." );
	}

	ImGui::BeginDisabled( !r_ssr.GetBool() );

	float ssrIntensity = r_ssrIntensity.GetFloat();
	if ( ImGui::SliderFloat( "Reflection Intensity", &ssrIntensity, 0.0f, 4.0f, "%.2f" ) ) {
		r_ssrIntensity.SetFloat( ssrIntensity );
	}
	AddTooltip( "r_ssrIntensity: overall reflection strength on top of the physical Fresnel weight. "
		"1 = physical; below dampens, above exaggerates the mirror look." );

	float ssrMaxRough = r_ssrMaxRoughness.GetFloat();
	if ( ImGui::SliderFloat( "Roughness Cutoff", &ssrMaxRough, 0.02f, 1.0f, "%.2f" ) ) {
		r_ssrMaxRoughness.SetFloat( ssrMaxRough );
	}
	AddTooltip( "r_ssrMaxRoughness: surfaces rougher than this reflect nothing (fade starts at 70% of it). "
		"Reflections are currently sharp-only, so keep this low — rough painted walls showing crisp mirror "
		"images reads wrong. Raise it to let more of the world reflect." );

	int ssrSteps = r_ssrSteps.GetInteger();
	if ( ImGui::SliderInt( "March Steps", &ssrSteps, 4, 64 ) ) {
		r_ssrSteps.SetInteger( ssrSteps );
	}
	AddTooltip( "r_ssrSteps: ray-march samples per pixel. More = longer, more complete reflections at "
		"higher GPU cost (watch r_gl3GpuTime). Fewer = cheaper, reflections cut off sooner." );

	float ssrDist = r_ssrMaxDistance.GetFloat();
	if ( ImGui::SliderFloat( "Max Distance", &ssrDist, 64.0f, 4096.0f, "%.0f" ) ) {
		r_ssrMaxDistance.SetFloat( ssrDist );
	}
	AddTooltip( "r_ssrMaxDistance: how far (world units) a reflection ray reaches. Rooms are ~256 units; "
		"1000 covers a large hall. Longer rays spread the same March Steps thinner." );

	float ssrThick = r_ssrThickness.GetFloat();
	if ( ImGui::SliderFloat( "Hit Thickness", &ssrThick, 1.0f, 128.0f, "%.0f" ) ) {
		r_ssrThickness.SetFloat( ssrThick );
	}
	AddTooltip( "r_ssrThickness: assumed depth of surfaces when testing ray hits. Too low leaves gaps "
		"in reflections (rays slip behind geometry); too high smears streaks under railings and edges." );

	float ssrFeedback = r_ssrTemporalFeedback.GetFloat();
	if ( ImGui::SliderFloat( "Temporal Feedback##ssr", &ssrFeedback, 0.0f, 0.97f, "%.2f" ) ) {
		r_ssrTemporalFeedback.SetFloat( ssrFeedback );
	}
	AddTooltip( "r_ssrTemporalFeedback: fraction of reflection history kept per frame while Temporal "
		"Accumulation is on (Graphics tab). Higher = smoother, converges slower and can trail "
		"on fast motion; lower = grainier but snappier." );

	float glassProbeScale = r_ssrGlassProbeScale.GetFloat();
	if ( ImGui::SliderFloat( "Glass Probe Intensity", &glassProbeScale, 0.0f, 4.0f, "%.2f" ) ) {
		r_ssrGlassProbeScale.SetFloat( glassProbeScale );
	}
	AddTooltip( "r_ssrGlassProbeScale: brightness of the baked room cubemap on glass only "
		"(Graphics > Glass Reflections). Applied on top of the material's own reflection "
		"colour and the global Reflection Brightness; other reflective surfaces are unaffected." );

	ImGui::EndDisabled();
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_DepthOfField()
{
	// Weapon-reload depth-of-field (r_dof): as a weapon reloads, the world beyond it
	// blurs so the view pulls focus onto the gun. Eases in on reload start, holds across
	// multi-shell reloads, eases out at the end. RHI backends only. The on/off switch
	// rides the group header; the sliders are live tuning.
	if ( BeginSettingsGroup( "Depth of Field (weapon reload)", &r_dof,
			"r_dof: as a weapon reloads, blur the world beyond it so the view pulls focus\n"
			"onto the gun. Eases in on reload start, holds across multi-shell reloads, eases\n"
			"out at the end. Non-vanilla; opengl3 / Vulkan only. Off = vanilla." ) ) {

		ImGui::BeginDisabled( !R_BackendSupportsEnhancements() );
		if ( !R_BackendSupportsEnhancements() ) {
			ImGui::TextDisabled( "Needs the opengl3 or Vulkan backend." );
		}

		float radius = r_dofBlurRadius.GetFloat();
		if ( ImGui::SliderFloat( "Blur Radius", &radius, 0.0f, 64.0f, "%.0f" ) ) {
			r_dofBlurRadius.SetFloat( radius );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofradius" ) ) { r_dofBlurRadius.SetFloat( 6.0f ); }
		AddTooltip( "r_dofBlurRadius: maximum world blur radius in pixels at full focus. "
			"Higher = stronger, dreamier blur; ~6 is a subtle default." );

		float focusStart = r_dofFocusStart.GetFloat();
		if ( ImGui::SliderFloat( "Focus Start", &focusStart, 0.0f, 1.0f, "%.2f" ) ) {
			r_dofFocusStart.SetFloat( focusStart );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofstart" ) ) { r_dofFocusStart.SetFloat( 0.35f ); }
		AddTooltip( "r_dofFocusStart: scene depth (0..1, hyperbolic) where the blur starts ramping. "
			"The weapon sits below this and stays sharp - raise it if the gun itself softens." );

		float focusEnd = r_dofFocusEnd.GetFloat();
		if ( ImGui::SliderFloat( "Focus End", &focusEnd, 0.0f, 1.0f, "%.2f" ) ) {
			r_dofFocusEnd.SetFloat( focusEnd );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofend" ) ) { r_dofFocusEnd.SetFloat( 0.72f ); }
		AddTooltip( "r_dofFocusEnd: scene depth where the blur reaches full strength. "
			"Lower it if the near world stays too sharp." );

		ImGui::Spacing();
		ImGui::Text( "Live reload focus: %.2f", r_weaponReloadFocus.GetFloat() );
		AddTooltip( "r_weaponReloadFocus: the current 0..1 envelope the game drives during a reload "
			"(read-only). Watch it rise to 1 while you reload and fall back to 0 after." );

		ImGui::EndDisabled();
		EndSettingsGroup();
	}
}

static void DrawDbgGroup_GpuOffload()
{
	// Vulkan GPU-offload experiments. All opt-in, off by default: they free CPU front-end
	// time (only a win when CPU-bound), and are groundwork for the RTX pivot (GPU-resident
	// skinned geometry so ray-tracing acceleration structures can be built/refit on-GPU).
	if ( BeginSettingsGroup( "GPU Offload (Vulkan)" ) ) {

		const bool vk = glConfig.rhiBackend && !glConfig.coreProfile;
		if ( !vk ) {
			ImGui::TextDisabled( "Vulkan backend only — switch to Vulkan in the Graphics tab (needs a renderer restart)." );
		}
		ImGui::BeginDisabled( !vk );

		bool gpuSkin = r_gpuSkinning.GetBool();
		if ( ImGui::Checkbox( "GPU skinning", &gpuSkin ) ) {
			r_gpuSkinning.SetBool( gpuSkin );
		}
		AddTooltip( "r_gpuSkinning: skin animated meshes on the GPU. Non-faithful (option-B TBN, not Doom 3's "
			"exact CPU skin), so off by default. Prerequisite for the CPU-skin strip below, and for ray-tracing "
			"animated geometry later. Takes effect on the next map load." );

		ImGui::BeginDisabled( !r_gpuSkinning.GetBool() );
		bool strip = r_gpuSkinStripCpu.GetBool();
		if ( ImGui::Checkbox( "Strip redundant CPU skin", &strip ) ) {
			r_gpuSkinStripCpu.SetBool( strip );
		}
		AddTooltip( "r_gpuSkinStripCpu: drop the now-redundant CPU position-skin for GPU-skinned surfaces. "
			"Frees CPU time — measured ~+3% fps when CPU-bound (weaker GPU / low presets); no change when "
			"GPU-bound. Needs GPU skinning on." );
		ImGui::EndDisabled();

		ImGui::Separator();

		int zfill = cvarSystem->GetCVarInteger( "r_vkBdaZfill" );
		if ( ImGui::Combo( "Z-fill offload", &zfill, "Off\0BDA vertex-fetch\0Batched indirect\0" ) ) {
			cvarSystem->SetCVarInteger( "r_vkBdaZfill", zfill );
		}
		AddTooltip( "r_vkBdaZfill: GPU-driven z-prepass via buffer-device-address vertex fetch (1) and batched "
			"indirect draws (2). Pixel-identical. Currently a wash / slight loss at typical geometry (batch "
			"overhead >= the draw-call savings) — kept as GPU-driven-rendering groundwork for RTX." );

		bool bdaVerbose = cvarSystem->GetCVarBool( "r_vkBdaVerbose" );
		if ( ImGui::Checkbox( "Verbose z-fill log", &bdaVerbose ) ) {
			cvarSystem->SetCVarBool( "r_vkBdaVerbose", bdaVerbose );
		}
		AddTooltip( "r_vkBdaVerbose: print the z-fill offload firing counter once/sec (per-draw / batched / "
			"fell-back). Off by default; a diagnostic to confirm the BDA path is active." );

		bool bdaTest = cvarSystem->GetCVarBool( "r_vkBdaTest" );
		if ( ImGui::Checkbox( "BDA self-test", &bdaTest ) ) {
			cvarSystem->SetCVarBool( "r_vkBdaTest", bdaTest );
		}
		AddTooltip( "r_vkBdaTest: one-shot buffer-device-address primitive self-test; prints PASS/FAIL to the console." );

		ImGui::Separator();

		bool gpuTime = cvarSystem->GetCVarBool( "r_vkGpuTime" );
		if ( ImGui::Checkbox( "Show GPU frame time", &gpuTime ) ) {
			cvarSystem->SetCVarBool( "r_vkGpuTime", gpuTime );
		}
		AddTooltip( "r_vkGpuTime: print the Vulkan GPU frame time (ms) once per second — handy for A/B-ing the offloads." );

		ImGui::EndDisabled();
		EndSettingsGroup();
	}
}

// Shadow Maps, Emissive, Glass, SSAO and Occlusion Maps are enhancement-backend features:
// each greys its OWN body out on the legacy renderer (BeginDisabled inside the group body,
// so the collapsible header itself stays usable and the sections can be reordered freely).
static void DrawDbgGroup_ShadowMaps()
{
	const bool supported = R_BackendSupportsEnhancements();
	if ( BeginSettingsGroup( "Shadow Maps" ) ) {

	if ( !supported ) {
		ImGui::TextDisabled( "Shadow mapping needs the GL 3.3 (opengl3) backend. Switch to it in the Graphics tab." );
	}
	ImGui::TextDisabled( "Per-light tuning for the shadow-map system (2D and cube maps)." );
	ImGui::Spacing();

	ImGui::BeginDisabled( !supported );

	bool sm = r_shadowMapping.GetBool();
	if ( ImGui::Checkbox( "Shadow Mapping (master)", &sm ) ) {
		r_shadowMapping.SetBool( sm );
	}
	AddTooltip( "Master toggle. On = shadow maps (stencil fully off); Off = vanilla stencil." );

	// isolate a single light by index to study its shadow in isolation; -1 = all
	int single = r_singleLight.GetInteger();
	if ( ImGui::InputInt( "Isolate Light (r_singleLight, -1=all)", &single ) ) {
		if ( single < -1 ) single = -1;
		r_singleLight.SetInteger( single );
	}
	AddTooltip( "Render only this light index (everything else black), to study one light's "
		"shadow. -1 shows all lights. Use r_shadowMapDebug 2 to read light indices in the console." );

	int dbg = r_shadowMapDebug.GetInteger();
	if ( ImGui::SliderInt( "Debug Print (r_shadowMapDebug)", &dbg, 0, 2 ) ) {
		r_shadowMapDebug.SetInteger( dbg );
	}
	AddTooltip( "0 = off, 1 = per-view summary, 2 = per-light readout (technique, occluder "
		"counts, dist/radius/range) printed to the console each frame." );

	ImGui::BeginDisabled( !r_shadowMapping.GetBool() );

	ImGui::SeparatorText( "Depth bias (acne vs peter-panning)" );

	float bias = r_shadowMapBias.GetFloat();
	if ( ImGui::SliderFloat( "Shadow Bias", &bias, 0.0f, 0.05f, "%.4f" ) ) {
		r_shadowMapBias.SetFloat( idMath::ClampFloat( 0.0f, 0.5f, bias ) );
	}
	AddTooltip( "Depth-compare bias (normalized by range). Raise to kill acne (stipple on lit "
		"surfaces); lower if shadows detach or vanish near contact (peter-panning). Slider caps "
		"at 0.05 for fine control; the cvar allows up to 0.5." );
	AddTooltip( "Bias for world/BSP and perforated receivers (flat surfaces)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##bias" ) ) {
		r_shadowMapBias.SetFloat( 0.0025f );
	}

	float modelBias = r_shadowMapModelBias.GetFloat();
	if ( ImGui::SliderFloat( "Model Bias", &modelBias, 0.0f, 0.05f, "%.4f" ) ) {
		r_shadowMapModelBias.SetFloat( idMath::ClampFloat( 0.0f, 0.5f, modelBias ) );
	}
	AddTooltip( "Separate bias for model (non-world) receivers. Curved model geometry "
		"self-shadows more, so it usually needs a larger bias than world surfaces." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##modelbias" ) ) {
		r_shadowMapModelBias.SetFloat( 0.005f );
	}

	int cull = r_shadowMapCull.GetInteger();
	const char *cullItems[] = { "front faces", "back faces (second-depth)", "two-sided" };
	if ( ImGui::Combo( "Caster Faces", &cull, cullItems, IM_ARRAYSIZE( cullItems ) ) ) {
		r_shadowMapCull.SetInteger( cull );
	}
	AddTooltip( "Which faces of occluders are rendered into the map. Back/second-depth (1) "
		"usually reduces acne; front (0) or two-sided (2) can fix light-leaks or thin-wall "
		"artifacts. Try these if shadows are missing/leaking on certain geometry." );

	bool perf = r_shadowMapPerforated.GetBool();
	if ( ImGui::Checkbox( "Perforated Casters (grates/fences)", &perf ) ) {
		r_shadowMapPerforated.SetBool( perf );
	}
	AddTooltip( "Alpha-tested grates/fences/foliage cast punched-out shadows." );

	ImGui::BeginDisabled( !perf );
	float perfStrength = r_shadowMapPerforatedStrength.GetFloat();
	if ( ImGui::SliderFloat( "Perforated Shadow Strength", &perfStrength, 0.0f, 1.0f, "%.2f" ) ) {
		r_shadowMapPerforatedStrength.SetFloat( perfStrength );
	}
	AddTooltip( "How dark grate/fence shadows get. 1 = fully dark like solid geometry; "
		"lower lets some light through, so a fence dims a room instead of blacking it "
		"out — closer to how vanilla faked these with soft light textures. Only affects "
		"perforated casters; solid-geometry shadows stay fully dark." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##perfstrength" ) ) {
		r_shadowMapPerforatedStrength.SetFloat( 0.5f );
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText( "Adaptive resolution (radius-scaled)" );

	bool sizeScale = r_shadowMapSizeScale.GetBool();
	if ( ImGui::Checkbox( "Scale Resolution With Light Size", &sizeScale ) ) {
		r_shadowMapSizeScale.SetBool( sizeScale );
	}
	AddTooltip( "Give bigger lights more shadow resolution and smaller lights less, so a "
		"shadow texel maps to roughly the same world distance for every light. Cuts jagged "
		"edges on large/far lights. Tiers step in powers of two around the base resolutions "
		"on the main Shadows page (-1x to +4x), clamped to each map's own range." );

	ImGui::BeginDisabled( !sizeScale );
	float refRadius = r_shadowMapSizeScaleRadius.GetFloat();
	if ( ImGui::SliderFloat( "Reference Radius", &refRadius, 16.0f, 2048.0f, "%.0f" ) ) {
		r_shadowMapSizeScaleRadius.SetFloat( refRadius );
	}
	AddTooltip( "The light radius that maps to the base resolution. Lights larger than this "
		"get more resolution; smaller ones get less. Lower it to push more lights into the "
		"higher-resolution tiers." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##refradius" ) ) {
		r_shadowMapSizeScaleRadius.SetFloat( 380.0f );
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText( "Cube map (point / omni lights)" );

	// Base 2D/cube resolutions and the point-light budget are owned by the main Shadows
	// page (one canonical control each) — only the dev-only cube tuning lives here.
	ImGui::TextDisabled( "Map resolutions and the point-light budget are on the main Shadows page." );
	ImGui::Spacing();

	int cubePcf = r_shadowMapCubePcf.GetInteger();
	if ( ImGui::SliderInt( "Cube Edge Softness (PCF taps)", &cubePcf, 1, 16 ) ) {
		r_shadowMapCubePcf.SetInteger( cubePcf );
	}
	AddTooltip( "Point-light cube shadows soften their edge by averaging this many disc-offset "
		"depth taps. 1 = a single hardware tap (hardest, blockiest, cheapest); 4 matches the "
		"2D/spot path; 6 (default) is a soft, well-filtered edge; higher smooths stair-stepping "
		"at low resolution but costs more per lit point-light fragment. Free on VRAM (filtering "
		"only)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##cubepcf" ) ) {
		r_shadowMapCubePcf.SetInteger( 6 );
	}

	bool faceCull = r_shadowMapFaceCull.GetBool();
	if ( ImGui::Checkbox( "Cull Off-Screen Faces", &faceCull ) ) {
		r_shadowMapFaceCull.SetBool( faceCull );
	}
	AddTooltip( "Skip rasterizing occluders into cube faces whose 90-degree cone can't reach "
		"the view frustum — those faces are never sampled by anything on screen. Big win at high "
		"resolution when a light is partly behind you. r_shadowMapDebug 1 shows faces drawn/culled." );

	float rscale = r_shadowMapPointRangeScale.GetFloat();
	if ( ImGui::SliderFloat( "Cube Range Scale", &rscale, 0.1f, 32.0f, "%.2fx" ) ) {
		r_shadowMapPointRangeScale.SetFloat( rscale );
	}
	AddTooltip( "Scales the cube shadow's far plane + depth normalizer. If distant/floor "
		"shadows under a light are missing, this controls how far the map reaches. 1.0 = the "
		"light's own radius." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##range" ) ) {
		r_shadowMapPointRangeScale.SetFloat( 1.0f );
	}

	ImGui::SeparatorText( "Large lights (sun replacements) -> stencil" );

	float stencilRadius = r_shadowMapStencilRadius.GetFloat();
	if ( ImGui::SliderFloat( "Stencil Fallback Radius", &stencilRadius, 0.0f, 512.0f,
			stencilRadius <= 0.0f ? "off" : "%.0f" ) ) {
		r_shadowMapStencilRadius.SetFloat( stencilRadius );
	}
	AddTooltip( "Lights whose largest radius axis exceeds this (world units) skip the shadow map and "
		"cast Carmack stencil shadows instead. Large 'sun' lights (Phobos fakes its sky with omni lights "
		"up to radius 5000) pixelate badly as one cube map and waste VRAM; stencil is pixel-exact at any "
		"distance and free of map memory. 0 = every light uses shadow maps. r_shadowMapDebug 1 shows the "
		"stencil-big count. (Console can set values above the slider max if you ever need them.)" );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##stencilradius" ) ) {
		r_shadowMapStencilRadius.SetFloat( 250.0f );
	}

	ImGui::SeparatorText( "Static cache (skip regenerating unchanged lights)" );

	bool cache = r_shadowMapCache.GetBool();
	if ( ImGui::Checkbox( "Cache Static Point Lights", &cache ) ) {
		r_shadowMapCache.SetBool( cache );
	}
	AddTooltip( "Keep each point light's cube map across frames and only re-render it when the "
		"light or one of its shadow casters moves. In mostly-static scenes this is the single "
		"biggest shadow-map speedup. r_shadowMapDebug 1 shows cache hit/rendered and MB used." );

	ImGui::BeginDisabled( !cache );
	const int vram = glConfig.vidMemMB;					// 0 if the vendor query failed
	const int cacheCvar = r_shadowMapCacheMB.GetInteger();
	bool autoBudget = ( cacheCvar < 0 );
	const int autoMB = ( vram > 0 ? vram / 2 : 1024 );
	const int sliderMax = ( vram > 0 ? vram : 16384 );

	if ( ImGui::Checkbox( "Auto Budget (half of VRAM)", &autoBudget ) ) {
		r_shadowMapCacheMB.SetInteger( autoBudget ? -1 : ( cacheCvar < 0 ? autoMB : cacheCvar ) );
	}
	if ( vram > 0 ) {
		AddTooltip( "Auto uses half of the detected video memory as the cache budget." );
	} else {
		AddTooltip( "Video-memory size couldn't be detected on this driver; auto falls back to 1024 MB." );
	}

	ImGui::BeginDisabled( autoBudget );
	int budget = ( cacheCvar < 0 ) ? autoMB : cacheCvar;
	if ( ImGui::SliderInt( "Cache Budget", &budget, 0, sliderMax, budget == 0 ? "unlimited" : "%d MB" ) ) {
		r_shadowMapCacheMB.SetInteger( budget );
	}
	AddTooltip( "VRAM the shadow cache may use. 0 = unlimited (cache every static light). Lights "
		"that don't fit fall back to per-frame regeneration, so lowering this never breaks shadows, "
		"it just caches fewer of them." );
	ImGui::EndDisabled();	// auto budget

	if ( vram > 0 ) {
		ImGui::TextDisabled( "Detected VRAM: %d MB   (effective budget: %s)",
			vram, autoBudget ? va( "%d MB (auto)", autoMB )
			                 : ( budget == 0 ? "unlimited" : va( "%d MB", budget ) ) );
	}
	ImGui::EndDisabled();	// cache on

	ImGui::EndDisabled();	// shadow mapping on
	ImGui::EndDisabled();	// backend supported
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_Emissive()
{
	const bool supported = R_BackendSupportsEnhancements();
	// --- Emissive surfaces (independent of shadow mapping; master toggle is in Graphics > Lighting) ---
	if ( BeginSettingsGroup( "Emissive Surfaces" ) ) {
	ImGui::TextDisabled( "Fill-light behaviour for glowing screens/monitors. Enable in Graphics > Lighting." );
	ImGui::Spacing();

	ImGui::BeginDisabled( !supported );
	ImGui::BeginDisabled( !r_emissiveSurfaces.GetBool() );

	float emScale = r_emissiveLightScale.GetFloat();
	if ( ImGui::SliderFloat( "Intensity", &emScale, 0.0f, 4.0f, "%.2f" ) ) {
		r_emissiveLightScale.SetFloat( emScale );
	}
	AddTooltip( "Brightness of the fill light a glowing screen casts. 0 = off, 0.50 = default, higher = a strong glow." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emscale" ) ) { r_emissiveLightScale.SetFloat( 0.50f ); }

	float emRadius = r_emissiveLightRadius.GetFloat();
	if ( ImGui::SliderFloat( "Reach (distance)", &emRadius, 0.25f, 16.0f, "%.2f" ) ) {
		r_emissiveLightRadius.SetFloat( emRadius );
	}
	AddTooltip( "How far the fill light reaches, as a multiple of the screen's own size. Small/medium screens "
		"scale linearly; large ones roll off toward the Max Reach cap below. Default 1.80." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emradius" ) ) { r_emissiveLightRadius.SetFloat( 1.80f ); }

	float emMaxReach = r_emissiveLightMaxReach.GetFloat();
	if ( ImGui::SliderFloat( "Max Reach (big signs)", &emMaxReach, 32.0f, 512.0f, "%.0f" ) ) {
		r_emissiveLightMaxReach.SetFloat( emMaxReach );
	}
	AddTooltip( "Soft cap (world units) that reach saturates toward, so a big hanging sign doesn't cast across "
		"the whole room. Lower = big signs reined in harder; raise toward 512 for near-linear scaling. Default 120." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emmaxreach" ) ) { r_emissiveLightMaxReach.SetFloat( 120.0f ); }

	float emFalloff = r_emissiveLightFalloff.GetFloat();
	if ( ImGui::SliderFloat( "Fade-off", &emFalloff, 0.0f, 1.0f, "%.2f" ) ) {
		r_emissiveLightFalloff.SetFloat( emFalloff );
	}
	AddTooltip( "How far the glow reaches and how gently it fades. 0 = a tight, bright pool with a sharp edge; "
		"1 = a soft glow that reaches further out. Default 0.5." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emfalloff" ) ) { r_emissiveLightFalloff.SetFloat( 0.5f ); }

	float emSpread = r_emissiveLightSpread.GetFloat();
	if ( ImGui::SliderFloat( "Beam Spread", &emSpread, 0.5f, 3.5f, "%.2f" ) ) {
		r_emissiveLightSpread.SetFloat( emSpread );
	}
	AddTooltip( "Width of the fill cone. 0.5 = a tight beam, 1.60 = default, 3.5 = a wide near-hemisphere "
		"that wraps around the screen (but still clipped behind the mount)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emspread" ) ) { r_emissiveLightSpread.SetFloat( 1.60f ); }

	float emSat = r_emissiveLightSaturation.GetFloat();
	if ( ImGui::SliderFloat( "Colour saturation", &emSat, 0.0f, 1.0f, "%.2f" ) ) {
		r_emissiveLightSaturation.SetFloat( emSat );
	}
	AddTooltip( "How much of the screen's own colour the bleed keeps. 0 = white, 0.75 = default tint, "
		"1 = full screen hue (a coloured spotlight)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emsat" ) ) { r_emissiveLightSaturation.SetFloat( 0.75f ); }

	ImGui::Spacing();

	bool emSpecular = r_emissiveLightSpecular.GetBool();
	if ( ImGui::Checkbox( "Specular Highlights", &emSpecular ) ) {
		r_emissiveLightSpecular.SetBool( emSpecular );
	}
	AddTooltip( "On: fill lights add a specular glint (more visible on your weapon). "
		"Off: diffuse-only, a calmer soft fill." );

	ImGui::EndDisabled();	// emissive surfaces on
	ImGui::EndDisabled();	// backend supported
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_Glass()
{
	const bool supported = R_BackendSupportsEnhancements();
	// --- Glass reflections (opengl3/Vulkan only) ---
	if ( BeginSettingsGroup( "Glass Reflections" ) ) {
	ImGui::TextDisabled( "Cube-map reflection (\"sheen\") brightness on glass. opengl3/Vulkan only." );
	ImGui::Spacing();

	ImGui::BeginDisabled( !supported );

	float reflScale = r_gl3ReflectionScale.GetFloat();
	if ( ImGui::SliderFloat( "Reflection brightness", &reflScale, 0.0f, 1.5f, "%.2f" ) ) {
		r_gl3ReflectionScale.SetFloat( reflScale );
	}
	AddTooltip( "Brightness of the environment-map reflection on glass. 1.00 = the untouched cube; "
		"0.70 = default, dampened 30% so the sheen matches legacy against the brighter enhanced scene." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##reflscale" ) ) { r_gl3ReflectionScale.SetFloat( 0.7f ); }

	ImGui::EndDisabled();	// glass reflections (backend supported)
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_SSAO()
{
	const bool supported = R_BackendSupportsEnhancements();
	// --- Ambient Occlusion (SSAO/GTAO). Master toggle is in Enhancements; full tuning here. ---
	if ( BeginSettingsGroup( "Ambient Occlusion (SSAO)" ) ) {
	ImGui::TextDisabled( "GTAO horizon-based ambient occlusion on the ambient term. Enable in Graphics > Ambient Occlusion." );
	ImGui::Spacing();

	ImGui::BeginDisabled( !supported );

	bool ssao = r_ssao.GetBool();
	if ( ImGui::Checkbox( "SSAO (master)", &ssao ) ) {
		r_ssao.SetBool( ssao );
	}
	AddTooltip( "Master toggle (same cvar as Graphics > Ambient Occlusion). On = occlude the "
		"ambient term in creases/contacts; Off = vanilla flat ambient." );

	int ssaoDbg = r_ssaoDebug.GetInteger();
	const char *ssaoDbgItems[] = { "off (feed lighting)", "show AO buffer", "show bent normals", "show normal G-buffer" };
	if ( ImGui::Combo( "Debug View (r_ssaoDebug)", &ssaoDbg, ssaoDbgItems, IM_ARRAYSIZE( ssaoDbgItems ) ) ) {
		r_ssaoDebug.SetInteger( ssaoDbg );
	}
	AddTooltip( "Visualize an SSAO buffer over the scene. 1 = the occlusion buffer (white = lit, "
		"dark = occluded); 2 = the bent normals as RGB; 3 = the raw normal G-buffer (needs Normal "
		"Buffer on) — here you should see the surface bump detail. Set back to 0 to feed lighting." );

	ImGui::BeginDisabled( !r_ssao.GetBool() );

	float ssaoInt = r_ssaoIntensity.GetFloat();
	if ( ImGui::SliderFloat( "AO Intensity", &ssaoInt, 0.0f, 4.0f, "%.2f" ) ) {
		r_ssaoIntensity.SetFloat( ssaoInt );
	}
	AddTooltip( "Strength of the darkening. 0 = none, 1.2 = default, higher pushes the occlusion deeper." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaoint" ) ) { r_ssaoIntensity.SetFloat( 1.2f ); }

	float ssaoDirect = r_ssaoDirectLight.GetFloat();
	if ( ImGui::SliderFloat( "Direct Light AO", &ssaoDirect, 0.0f, 1.0f, "%.2f" ) ) {
		r_ssaoDirectLight.SetFloat( ssaoDirect );
	}
	AddTooltip( "How strongly AO darkens direct (dynamic) light's diffuse. Doom 3 has almost no "
		"ambient, so THIS is what makes AO visible in normal gameplay. 1 = full, 0.75 = default, 0 = "
		"ambient-only (most faithful, but usually invisible here). Lower it if AO looks baked-in "
		"when lights move." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaodirect" ) ) { r_ssaoDirectLight.SetFloat( 0.75f ); }

	float ssaoFloor = r_ssaoFloor.GetFloat();
	if ( ImGui::SliderFloat( "Floor (min visibility)", &ssaoFloor, 0.0f, 1.0f, "%.2f" ) ) {
		r_ssaoFloor.SetFloat( ssaoFloor );
	}
	AddTooltip( "Anti-crush floor: how dark a fully-occluded spot may get. 0 = can reach black, "
		"0.03 = default, 1 = no darkening. Raise it if AO makes dark areas too murky to read." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaofloor" ) ) { r_ssaoFloor.SetFloat( 0.03f ); }

	float ssaoRad = r_ssaoRadius.GetFloat();
	if ( ImGui::SliderFloat( "Radius (world units)", &ssaoRad, 1.0f, 256.0f, "%.0f" ) ) {
		r_ssaoRadius.SetFloat( ssaoRad );
	}
	AddTooltip( "How far the occlusion samples reach, in world units. Small = tight contact creases "
		"only; large = broad, softer occlusion (and more expensive). Default 48." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaorad" ) ) { r_ssaoRadius.SetFloat( 48.0f ); }

	int ssaoSlices = r_ssaoSlices.GetInteger();
	if ( ImGui::SliderInt( "Directions (slices)", &ssaoSlices, 1, 8 ) ) {
		r_ssaoSlices.SetInteger( ssaoSlices );
	}
	AddTooltip( "How many horizon-search directions per pixel. More = smoother, less directional "
		"noise. This is the bigger cost knob. Default 3. Check the cost with r_gl3GpuTime 1." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaoslices" ) ) { r_ssaoSlices.SetInteger( 3 ); }

	int ssaoSteps = r_ssaoSteps.GetInteger();
	if ( ImGui::SliderInt( "Steps per direction", &ssaoSteps, 1, 12 ) ) {
		r_ssaoSteps.SetInteger( ssaoSteps );
	}
	AddTooltip( "How many samples are marched along each direction (how finely each horizon is "
		"found). More = more accurate occlusion at range. Default 3." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaosteps" ) ) { r_ssaoSteps.SetInteger( 3 ); }

	// Depth-mip acceleration + its two quality/speed knobs (docs/ssao-perf-optimization.md).
	// The master toggle is mirrored from Enhancements; the bias/cap sliders live only here.
	bool ssaoDepthMipDev = r_ssaoDepthMip.GetBool();
	if ( ImGui::Checkbox( "Depth-Mip Acceleration##dev", &ssaoDepthMipDev ) ) {
		r_ssaoDepthMip.SetBool( ssaoDepthMipDev );
	}
	AddTooltip( "March the horizon search over a prefiltered linear-depth mip chain (far taps read "
		"coarse mips) instead of full-res depth every tap — cheaper at a wide radius. Same toggle as "
		"Graphics > Depth-Mip Acceleration." );

	ImGui::BeginDisabled( !r_ssaoDepthMip.GetBool() );
	float ssaoMipBias = r_ssaoDepthMipBias.GetFloat();
	if ( ImGui::SliderFloat( "Mip Bias", &ssaoMipBias, 0.05f, 1.0f, "%.2f" ) ) {
		r_ssaoDepthMipBias.SetFloat( ssaoMipBias );
	}
	AddTooltip( "How eagerly the march drops to coarser mips: LOD = log2(stepPixels * this). Higher = "
		"coarser sooner (faster, but coarse depth smears occlusion across silhouettes into halos); "
		"lower = stays on finer mips (sharper, keeps most of the speedup). Default 0.1." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaomipbias" ) ) { r_ssaoDepthMipBias.SetFloat( 0.1f ); }

	int ssaoMipCap = r_ssaoDepthMipMaxLod.GetInteger();
	if ( ImGui::SliderInt( "Mip Coarseness Cap", &ssaoMipCap, 1, 5 ) ) {
		r_ssaoDepthMipMaxLod.SetInteger( ssaoMipCap );
	}
	AddTooltip( "Caps how coarse the march may ever go. The coarsest mips (box-averaged depth) are "
		"where occlusion smears across silhouettes into halos, so a lower cap kills halos at a small "
		"speed cost; 5 = uncapped (old behaviour). Default 2." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaomipcap" ) ) { r_ssaoDepthMipMaxLod.SetInteger( 2 ); }
	ImGui::EndDisabled();

	bool ssaoTemporalDev = r_ssaoTemporal.GetBool();
	if ( ImGui::Checkbox( "Temporal Accumulation", &ssaoTemporalDev ) ) {
		r_ssaoTemporal.SetBool( ssaoTemporalDev );
	}
	AddTooltip( "Reuse the previous frame's AO (reprojected by camera motion) to amortize the "
		"horizon search across frames: smooths the per-pixel noise and lets Directions/Steps run "
		"lower for the same look. Ghosting on fast motion / disocclusion is bounded by a "
		"neighbourhood clamp. Same toggle as Graphics > Ambient Occlusion." );

	ImGui::BeginDisabled( !r_ssaoTemporal.GetBool() );
	float ssaoFeedback = r_ssaoTemporalFeedback.GetFloat();
	if ( ImGui::SliderFloat( "Temporal Feedback", &ssaoFeedback, 0.0f, 0.97f, "%.2f" ) ) {
		r_ssaoTemporalFeedback.SetFloat( ssaoFeedback );
	}
	AddTooltip( "Fraction of the reprojected previous-frame AO kept each frame. Higher = smoother "
		"and steadier (and effectively cheaper) but more latency/ghosting; 0 = no accumulation. "
		"Default 0.90." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaofeedback" ) ) { r_ssaoTemporalFeedback.SetFloat( 0.9f ); }
	ImGui::EndDisabled();

	// Resolution is exposed in Graphics > Ambient Occlusion (as a slider).

	bool ssaoNormalBuf = r_ssaoNormalBuffer.GetBool();
	if ( ImGui::Checkbox( "Normal Buffer (bump-mapped)", &ssaoNormalBuf ) ) {
		r_ssaoNormalBuffer.SetBool( ssaoNormalBuf );
	}
	AddTooltip( "Feed SSAO from a real bump-mapped normal G-buffer (an extra opaque geometry pass) "
		"instead of normals reconstructed from depth. Picks up normal-map surface detail and removes "
		"faceting (see Debug View 3), at the cost of one geometry pass. Off = reconstruct from depth "
		"(cheaper). Foundation for parallax-occlusion / displacement later." );

	bool ssaoBent = r_ssaoBentNormal.GetBool();
	if ( ImGui::Checkbox( "Bent Normals", &ssaoBent ) ) {
		r_ssaoBentNormal.SetBool( ssaoBent );
	}
	AddTooltip( "Shade the AMBIENT light along the bent normal (average unoccluded direction) so it "
		"responds to macro occlusion, not just the surface normal. Visible in Debug View 2. Note: "
		"only bites where there IS ambient light, which is sparse in Doom 3, so the effect is subtle." );

	ImGui::BeginDisabled( !r_ssaoBentNormal.GetBool() );
	float ssaoBentStr = r_ssaoBentStrength.GetFloat();
	if ( ImGui::SliderFloat( "Bent Strength", &ssaoBentStr, 0.0f, 1.0f, "%.2f" ) ) {
		r_ssaoBentStrength.SetFloat( ssaoBentStr );
	}
	AddTooltip( "How far the ambient lookup bends from the surface normal toward the bent normal. "
		"0 = surface normal (no effect), 1 = fully bent. Default 0.5." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaobentstr" ) ) { r_ssaoBentStrength.SetFloat( 0.5f ); }
	ImGui::EndDisabled();

	bool ssaoSpec = r_ssaoSpecular.GetBool();
	if ( ImGui::Checkbox( "Specular Occlusion", &ssaoSpec ) ) {
		r_ssaoSpecular.SetBool( ssaoSpec );
	}
	AddTooltip( "Also dim specular highlights in occluded areas on direct lights (stronger "
		"anti-plastic look; a mild deviation from vanilla). Scaled by Direct Light AO. Off by default." );

	ImGui::EndDisabled();	// ssao on
	ImGui::EndDisabled();	// backend supported
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_OcclusionMaps()
{
	const bool supported = R_BackendSupportsEnhancements();
	// Baked occlusion maps (docs/occlusion-maps.md). Independent of SSAO (works with it off);
	// still enhancement-backend only. Mirrors the Enhancements-tab toggle and adds the two
	// strength sliders.
	if ( BeginSettingsGroup( "Occlusion Maps" ) ) {
		ImGui::BeginDisabled( !supported );
		bool oclMaps = r_occlusionMaps.GetBool();
		if ( ImGui::Checkbox( "Baked Occlusion Maps (r_occlusionMaps)", &oclMaps ) ) {
			r_occlusionMaps.SetBool( oclMaps );
		}
		AddTooltip( "Per-material baked ambient-occlusion textures (the `occlusionmap` material "
			"stage), applied to the ambient and direct-light diffuse like SSAO. Inert unless a "
			"material ships an occlusion map; no stock Doom 3 asset does, so this targets mods." );

		ImGui::BeginDisabled( !r_occlusionMaps.GetBool() );
		float oclScale = r_occlusionMapScale.GetFloat();
		if ( ImGui::SliderFloat( "AO Map Strength", &oclScale, 0.0f, 1.0f, "%.2f" ) ) {
			r_occlusionMapScale.SetFloat( oclScale );
		}
		AddTooltip( "How strongly the occlusion map darkens the ambient term. 0 = off, 1 = the map "
			"at full darkening. Default 1.0." );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##oclscale" ) ) { r_occlusionMapScale.SetFloat( 1.0f ); }

		float oclDirect = r_occlusionMapDirect.GetFloat();
		if ( ImGui::SliderFloat( "Direct Light AO (map)", &oclDirect, 0.0f, 1.0f, "%.2f" ) ) {
			r_occlusionMapDirect.SetFloat( oclDirect );
		}
		AddTooltip( "How strongly the occlusion map darkens direct (dynamic) light's diffuse, as a "
			"fraction of AO Map Strength. Doom 3 is mostly dynamic light, so this is what makes the "
			"map visible; lower it if AO looks baked-in under moving lights. 0 = ambient-only. Default 0.9." );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##ocldirect" ) ) { r_occlusionMapDirect.SetFloat( 0.9f ); }
		ImGui::EndDisabled();
		ImGui::EndDisabled();	// backend supported
		EndSettingsGroup();
	}
}

static void DrawShadowDebugMenu()
{
	ImGui::TextDisabled( "Developer tools for inspecting and tuning the renderer live: general debug "
		"views, shadow maps, and emissive surfaces. Dial values in, then report the good ones back." );
	ImGui::Spacing();

	// Collapsible "burger" groups, ordered to mirror the Graphics tab. Reorder these
	// calls to change the on-screen order.
	DrawDbgGroup_RenderDebugging();
	DrawDbgGroup_ShadowMaps();
	DrawDbgGroup_SSAO();
	DrawDbgGroup_OcclusionMaps();
	DrawDbgGroup_Tessellation();
	DrawDbgGroup_PBR();
	DrawDbgGroup_Emissive();
	DrawDbgGroup_SSR();
	DrawDbgGroup_DepthOfField();
	DrawDbgGroup_Glass();
	DrawDbgGroup_GpuOffload();
}

static idStrList alDevices;
static int selAlDevice = 0;
static float audioMenuItemOffset = 0.0f;

static void InitAudioOptionsMenu()
{
	alDevices.SetNum( 0, false );
	selAlDevice = 0; // default device (another one might be set in loop below)

	const char* device = idSoundSystemLocal::s_device.GetString();
	if ( *device == '\0' || idStr::Cmp( device, "default" ) == 0 ) {
		device = nullptr;
	}

	alDevices.Append( idStr( "(System's default sound device)" ) );

	if ( idSoundSystemLocal::alEnumerateAllAvailable ) {
		const char *devs = alcGetString( NULL, ALC_ALL_DEVICES_SPECIFIER );

		while (devs && *devs) {
			if ( device && !idStr::Icmp(devs, device) ) {
				selAlDevice = alDevices.Num();
			}

			alDevices.Append( idStr( devs ) );

			devs += strlen(devs) + 1;
		}
	}

	audioMenuItemOffset = ImGui::CalcTextSize( "Strength of EFX Reverb Effects (?)" ).x;
	audioMenuItemOffset += ImGui::GetStyle().ItemSpacing.x;
}

static void DrawAudioOptionsMenu()
{
	float itemWidth = ImGui::GetContentRegionAvail().x - audioMenuItemOffset;
	ImGui::PushItemWidth( itemWidth );

	ALCdevice* alDevice = soundSystemLocal.openalDevice;

	if ( alDevice == nullptr ) {
		if ( soundSystemLocal.s_noSound.GetBool() ) {
			ImGui::TextDisabled( "Sound is disabled with s_noSound, so there's nothing to show here!" );
		} else {
			ImGui::TextDisabled( "!!! No OpenAL device is opened, so there is no sound !!!" );
		}
		return;
	}

	ImGui::SeparatorText( "Settings that require restarting DUDE" );

	if ( ImGui::Combo( "Sound Device", &selAlDevice, [](void* data, int idx) -> const char* {
			const idStrList& devs = *static_cast< const idStrList* >(data);
			return devs[idx].c_str();
		}, &alDevices, alDevices.Num() ) )
	{
		if ( selAlDevice == 0 ) {
			idSoundSystemLocal::s_device.SetString( "default" );
		} else {
			idSoundSystemLocal::s_device.SetString( alDevices[selAlDevice] );
		}
		D3::ImGuiHooks::ShowWarningOverlay( "Changing the sound device only takes effect after restarting DUDE!" );
	}
	AddTooltip( "s_device" );

	if ( !idSoundSystemLocal::EFXAvailable ) {
		ImGui::BeginDisabled();
		bool b = false;
		ImGui::Checkbox( "Use EAX/EFX Reverb Effects", &b );
		AddTooltip( "EFX effects are not available in your OpenAL implementation.\nConsider using OpenAL-Soft." );
		ImGui::EndDisabled();
	} else {
		bool useReverb = idSoundSystemLocal::s_useEAXReverb.GetBool();
		if ( ImGui::Checkbox( "Use EAX/EFX Reverb Effects", &useReverb ) ) {
			idSoundSystemLocal::s_useEAXReverb.SetBool( useReverb );
			if ( useReverb != idSoundSystemLocal::useEFXReverb ) {
				D3::ImGuiHooks::ShowWarningOverlay( "Enabling/disabling EFX only takes effect after restarting DUDE!" );
			}
		}
		AddTooltip( "s_useEAXReverb" );
	}

	ImGui::SeparatorText( "Settings that take effect immediately" );

	float vol = idSoundSystemLocal::s_volume.GetFloat(); // cvar is called s_volume_dB
	if ( vol == 0.0f && signbit(vol) != 0 ) {
		vol = 0.0f; // turn -0.0 into 0.0
	}
	if ( ImGui::SliderFloat( "Volume", &vol, -40, 10, "%.1f" ) ) {
		idSoundSystemLocal::s_volume.SetFloat( vol );
	}
	AddTooltip( "s_volume_dB" );
	AddDescrTooltip( "The game's main volume in dB. 0 is the regular maximum volume" );

	bool scaleDownAndClamp = idSoundSystemLocal::s_scaleDownAndClamp.GetBool();
	if ( ImGui::Checkbox( "Scale down and clamp all sound volumes", &scaleDownAndClamp ) ) {
		idSoundSystemLocal::s_scaleDownAndClamp.SetBool( scaleDownAndClamp );
	}
	AddCVarOptionTooltips( idSoundSystemLocal::s_scaleDownAndClamp );

	if ( idSoundSystemLocal::alOutputLimiterAvailable ) {
		int outLimSel = 1 + idMath::ClampInt( -1, 1, idSoundSystemLocal::s_alOutputLimiter.GetInteger() );
		if ( ImGui::Combo( "OpenAL output limiter", &outLimSel, "Auto (let OpenAL decide)\0Disabled\0Enabled\0" ) ) {
			idSoundSystemLocal::s_alOutputLimiter.SetInteger( outLimSel - 1 );
		}
		AddCVarOptionTooltips( idSoundSystemLocal::s_alOutputLimiter );
	} else {
		ImGui::BeginDisabled();
		int c = 0;
		ImGui::Combo( "OpenAL output limiter", &c, "Auto (let OpenAL decide)\0" );
		AddTooltip( "Your OpenAL version doesn't support configuring output-limiter (needs ALC_SOFT_output_limiter extension)" );
		ImGui::EndDisabled();
	}

	if ( idSoundSystemLocal::alHRTFavailable ) {
		int hrtfSel = 1 + idMath::ClampInt( -1, 1, idSoundSystemLocal::s_alHRTF.GetInteger() );
		if ( ImGui::Combo( "Use HRTF", &hrtfSel, "Auto (let OpenAL decide)\0Disabled\0Enabled\0" ) ) {
			idSoundSystemLocal::s_alHRTF.SetInteger( hrtfSel - 1 );
		}
		AddCVarOptionTooltips( idSoundSystemLocal::s_alHRTF );
	} else {
		ImGui::BeginDisabled();
		int c = 0;
		ImGui::Combo( "Use HRTF", &c, "Auto (let OpenAL decide)\0" );
		AddTooltip( "Your OpenAL version doesn't support configuring HRTF (needs ALC_SOFT_HRTF extension)" );
		ImGui::EndDisabled();
	}

	if ( idSoundSystemLocal::useEFXReverb ) {
		float reverbGain = idSoundSystemLocal::s_alReverbGain.GetFloat();
		if ( ImGui::SliderFloat( "Strength of EFX Reverb Effects", &reverbGain, 0, 1,
				"%.2f", ImGuiSliderFlags_AlwaysClamp ) )
		{
			idSoundSystemLocal::s_alReverbGain.SetFloat( reverbGain );
		}
		AddTooltip( "s_alReverbGain" );
		AddDescrTooltip( "the default value is 0.5" );
	} else {
		ImGui::BeginDisabled();
		float f = 0.0f;
		ImGui::SliderFloat( "Strength of EFX Reverb Effects", &f, 0, 1, "Disabled" );
		if ( !idSoundSystemLocal::EFXAvailable ) {
			AddTooltip( "Your OpenAL version doesn't support EFX" );
		} else {
			AddTooltip( "EFX reverb effects are currently disabled" );
		}
		ImGui::EndDisabled();
	}

	bool playDefaultSound = idSoundSystemLocal::s_playDefaultSound.GetBool();
	if ( ImGui::Checkbox( "Play default beep for missing sounds", &playDefaultSound ) ) {
		idSoundSystemLocal::s_playDefaultSound.SetBool( playDefaultSound );
	}
	AddCVarOptionTooltips( idSoundSystemLocal::s_playDefaultSound );

	ImGui::Separator();

	if ( ImGui::TreeNode("OpenAL Info") ) {
		ImGui::BeginDisabled();

		ImGui::Text( "OpenAL vendor: %s", alGetString(AL_VENDOR) );
		ImGui::Text( "OpenAL renderer: %s", alGetString(AL_RENDERER) );
		ImGui::Text( "OpenAL version: %s", alGetString(AL_VERSION) );

		if ( idSoundSystemLocal::alEnumerateAllAvailable ) {
			ImGui::Text( "Current playback device: %s", alcGetString( alDevice, ALC_ALL_DEVICES_SPECIFIER ) );
			ImGui::Text( "Default playback device: %s", alcGetString( nullptr, ALC_DEFAULT_ALL_DEVICES_SPECIFIER ) );
		} else {
			ImGui::Text( "Current playback device: %s", alcGetString( alDevice, ALC_DEVICE_SPECIFIER ) );
			ImGui::Text( "Default playback device: %s", alcGetString( nullptr, ALC_DEFAULT_DEVICE_SPECIFIER) );
		}

		if ( idSoundSystemLocal::alIsDisconnectAvailable ) {
			ALCint connected;
			alcGetIntegerv( alDevice, ALC_CONNECTED, 1, &connected );
			ImGui::Text( "The device is %s", connected ? "Connected" : "! Disconnected !" );
		}

		ALCint sampleRate = 0;
		alcGetIntegerv( alDevice, ALC_FREQUENCY, 1, &sampleRate );

		if ( idSoundSystemLocal::alOutputModeAvailable ) {
			const char* modeName = "Unknown / Unspecified Mode";
			ALCint mode = 0;
			alcGetIntegerv(alDevice, ALC_OUTPUT_MODE_SOFT, 1, &mode);

			switch ( mode ) {
				//case ALC_ANY_SOFT : break; // Unknown is already the default string
				case ALC_MONO_SOFT         : modeName = "Mono (1 channel)"; break;
				case ALC_STEREO_SOFT       : modeName = "Stereo (2 channels)"; break;
				case ALC_STEREO_BASIC_SOFT : modeName = "Basic Stereo"; break;
				case ALC_STEREO_UHJ_SOFT   :
					modeName = "Stereo-compatible 2-channel UHJ surround encoding";
					break;
				case ALC_STEREO_HRTF_SOFT  : modeName = "Stereo with HRTF"; break;
				case ALC_QUAD_SOFT         : modeName = "Quadraphonic (4 channels)"; break;
				case ALC_SURROUND_5_1_SOFT : modeName = "5.1 Surround"; break;
				case ALC_SURROUND_6_1_SOFT : modeName = "6.1 Surround"; break;
				case ALC_SURROUND_7_1_SOFT : modeName = "7.1 Surround"; break;
			}

			ImGui::Text( "Device's Playback Mode: %s with samplerate %dHz", modeName, sampleRate );
		} else {
			ImGui::Text( "Devices's Samplerate: %dHz", sampleRate );
		}

		if ( idSoundSystemLocal::alOutputLimiterAvailable ) {
			ALCint limiterState = 0;
			alcGetIntegerv( alDevice, ALC_OUTPUT_LIMITER_SOFT, 1, &limiterState );
			ImGui::Text( "Output-Limiter: %s", limiterState ? "Enabled" : "Disabled" );
		} else {
			ImGui::TextUnformatted( "(Output-Limiter extension ALC_SOFT_output_limiter not available)" );
		}

		if ( idSoundSystemLocal::alHRTFavailable ) {
			ALCint hrtfEnabled = 0;
			alcGetIntegerv( alDevice, ALC_HRTF_SOFT, 1, &hrtfEnabled );
			ImGui::Text( "HRTF is: %s", hrtfEnabled ? "Enabled" : "Disabled" );

			ImGui::Indent();
			ALCint status = 0;
			alcGetIntegerv( alDevice, ALC_HRTF_STATUS_SOFT, 1, &status );
			switch ( status ) {
				case ALC_HRTF_DISABLED_SOFT:
				case ALC_HRTF_ENABLED_SOFT:
					break;
				case ALC_HRTF_DENIED_SOFT:
					ImGui::TextWrapped( "HRTF is disabled because it's not allowed on the device, or disabled in the config (alsoft.conf/alsoftrc/alsoft.ini - ALC_HRTF_DENIED_SOFT)" );
					break;
				case ALC_HRTF_REQUIRED_SOFT:
					ImGui::TextWrapped( "HRTF is enabled because it's required, either by the hardware or by the config (alsoft.conf/alsoftrc/alsoft.ini - ALC_HRTF_REQUIRED_SOFT)" );
					break;
				case ALC_HRTF_HEADPHONES_DETECTED_SOFT:
					ImGui::TextWrapped( "HRTF is enabled because the device was detected as headphones (ALC_HRTF_HEADPHONES_DETECTED_SOFT)" );
					break;
				case ALC_HRTF_UNSUPPORTED_FORMAT_SOFT:
					ImGui::TextWrapped( "HRTF is disabled because it's unsupported with the current format, e.g. not stereo (ALC_HRTF_UNSUPPORTED_FORMAT_SOFT)" );
					break;
				default:
					ImGui::Text( "Not sure why HRTF is %s, unknown status value %d!", hrtfEnabled ? "enabled" : "disabled ", status );
			}
			ImGui::Unindent();
		}

		ImGui::EndDisabled();
		ImGui::TreePop();
	} else {
		AddTooltip( "Click to show information about the current OpenAL device" );
	}
}

static CVarOption gameOptions[] = {
	CVarOption( "Movement and Weapons" ),
	CVarOption( "in_alwaysRun", "Always Run (Multiplayer-only by default)", OT_BOOL ),
	CVarOption( "in_allowAlwaysRunInSP", "Allow Always Run and Toggle Run in Singleplayer\n(Stamina is still limited!)", OT_BOOL ),
	CVarOption( "in_toggleRun", "Toggle Run (Multiplayer-only by default)", OT_BOOL ),
	CVarOption( "in_toggleCrouch", "Toggle Crouch", OT_BOOL ),
	CVarOption( "in_toggleZoom", "Toggle Zoom", OT_BOOL ),
	CVarOption( "ui_autoReload", "Auto Weapon Reload", OT_BOOL ),
	CVarOption( "ui_autoSwitch", "Auto Weapon Switch", OT_BOOL ),
	CVarOption( "Visual" ),
	CVarOption( "g_showHud", "Show HUD", OT_BOOL ),
	CVarOption( "com_showFPS", []( idCVar& cvar ) {
		int curFormat = idMath::ClampInt( 0, 2, cvar.GetInteger() );
		const char* choices = "Don't show framerate\0Show only framerate\0Show framerate and frame times (avg/min/max)\0";
		if ( ImGui::Combo( "Show Framerate (FPS)", &curFormat, choices ) ) {
			cvar.SetInteger( curFormat );
		}
		AddTooltip( "com_showFPS" );
		AddDescrTooltip( cvar.GetDescription() );
	} ),
	CVarOption( "ui_showGun", "Show Gun Model", OT_BOOL ),
	CVarOption( "g_decals", "Show Decals", OT_BOOL ),
	CVarOption( "g_bloodEffects", "Show Blood and Gibs", OT_BOOL ),
	CVarOption( "g_doubleVision", "Show Double Vision when Taking Damage", OT_BOOL ),
	CVarOption( "g_hitEffect", "Mess Up Player Camera when Taking Damage", OT_BOOL ),
	CVarOption( "con_noPrint", "Print console output only to console, don't show when it's closed", OT_BOOL ),
};

static char playerNameBuf[128] = {};
static idCVar* ui_nameVar = nullptr;

void InitGameOptionsMenu()
{
	ui_nameVar = cvarSystem->Find( "ui_name" );

	// Note: ImGui uses UTF-8 for strings, Doom3 uses ISO8859-1, so we need to translate
	if ( D3_ISO8859_1toUTF8( ui_nameVar->GetString(), playerNameBuf, sizeof(playerNameBuf) ) == nullptr ) {
		// returning NULL means playerNameBuf wasn't big enough - that shouldn't happen,
		// at least the player name input in the original menu only allowed 40 chars
		playerNameBuf[sizeof(playerNameBuf)-1] = '\0';
	}

	InitOptions( gameOptions, IM_ARRAYSIZE(gameOptions) );
}

static int PlayerNameInputTextCallback(ImGuiInputTextCallbackData* data)
{
	if ( data->EventChar > 0xFF ) { // some unicode char that ISO8859-1 can't represent
		data->EventChar = 0;
		return 1;
	}

	if ( data->Buf ) {
		// we want at most 40 codepoints
		int newLen = D3_UTF8CutOffAfterNCodepoints( data->Buf, 40 );
		if ( newLen != data->BufTextLen ) {
			data->BufTextLen = newLen;
			data->BufDirty = true;
		}
	}

	return 0;
}

void DrawGameOptionsMenu()
{
	ImGui::Spacing();

	ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCharFilter;
	if ( ImGui::InputText( "Player Name", playerNameBuf, sizeof(playerNameBuf), flags, PlayerNameInputTextCallback )
	     && playerNameBuf[0] != '\0' )
	{
		char playerNameIso[128] = {};
		if ( D3_UTF8toISO8859_1( playerNameBuf, playerNameIso, sizeof(playerNameIso), '!' ) != NULL ) {
			playerNameIso[40] = '\0'; // limit to 40 chars, like the original menu
			ui_nameVar->SetString( playerNameIso );
			// update the playerNameBuf to reflect the name as it is now: limited to 40 chars
			// and possibly containing '!' from non-translatable unicode chars
			D3_ISO8859_1toUTF8( ui_nameVar->GetString(), playerNameBuf, sizeof(playerNameBuf) );
		} else {
			D3::ImGuiHooks::ShowWarningOverlay( "Player Name way too long (max 40 chars) or contains invalid UTF-8 encoding!" );
			playerNameBuf[0] = '\0';
		}
	}
	AddTooltip( "ui_name" );

	DrawOptions( gameOptions, IM_ARRAYSIZE(gameOptions) );
}


static bool showStyleEditor = false;

static void DrawOtherOptionsMenu()
{
	ImGui::Spacing();
	float scale = D3::ImGuiHooks::GetScale();
	if ( ImGui::DragFloat("ImGui scale", &scale, 0.005f, 0.25f, 8.0f, "%.3f") ) {
		D3::ImGuiHooks::SetScale( scale );
	}
	ImGui::SameLine();
	if ( ImGui::Button("Reset") ) {
		D3::ImGuiHooks::SetScale( -1.0f );
	}

	int style_idx = imgui_style.GetInteger();
	if ( ImGui::Combo( "ImGui Style", &style_idx, "DUDE\0ImGui Default\0Userstyle\0") )
	{
		switch( style_idx )
		{
			case 0: D3::ImGuiHooks::SetImGuiStyle( D3::ImGuiHooks::Style::Dhewm3 ); break;
			case 1: D3::ImGuiHooks::SetImGuiStyle( D3::ImGuiHooks::Style::ImGui_Default ); break;
			case 2: D3::ImGuiHooks::SetImGuiStyle( D3::ImGuiHooks::Style::User ); break;
		}
		imgui_style.SetInteger( style_idx );
	}

	ImGui::Spacing();

	ImGui::Checkbox( "Show Dear ImGui Style Editor", &showStyleEditor );

	ImGui::SameLine();

	if ( ImGui::Button( "Write Userstyle" ) ) {
		D3::ImGuiHooks::WriteUserStyle();
		imgui_style.SetInteger( 2 );
	}
	AddTooltip( "Writes the current style settings (incl. colors) as userstyle" );

	static bool onlyChanges = false;
	if ( ImGui::Button( "Copy style code to clipboard" ) ) {
		D3::ImGuiHooks::CopyCurrentStyle( onlyChanges );
	}
	AddTooltip( "Generates C++ code for the current style settings (incl. colors) and copies it into the clipboard" );

	ImGui::SameLine();

	ImGui::Checkbox( "Only changed settings", &onlyChanges );
	AddTooltip( "Only generate C++ code for attributes/colors that are changed compared to the default (dark) ImGui theme" );

	ImGui::Spacing();

	if ( ImGui::Button( "Show ImGui Demo" ) ) {
		D3::ImGuiHooks::OpenWindow( D3::ImGuiHooks::D3_ImGuiWin_Demo );
	}
}

} //anon namespace

// DUDE: bridge so the classic Doom 3 quality selector (base/guis/mainmenu.gui) and
// the console can drive the enhancement presets. Defined outside the anonymous
// namespace above so it has external linkage (Common.cpp registers the command);
// ApplyEnhancementPreset stays visible from here.
//
//   dudePreset <n>   set the dude_preset cvar to tier n and apply it
//   dudePreset       apply whatever tier dude_preset currently holds
//
// The no-arg form is what the in-game selector uses: its choiceDef writes dude_preset
// live, then fires "exec dudePreset" so we apply the just-written value. Applying pins
// com_machineSpec to Ultra (3) - the legacy image-quality classification is no longer
// user-selectable (every GPU we support handles Ultra), so base texture/image quality
// stays maxed rather than leaving a stale low value behind.
void Com_DudePreset_f( const idCmdArgs &args )
{
	int idx;
	if ( args.Argc() >= 2 ) {
		idx = atoi( args.Argv( 1 ) );
		if ( idx < 0 || idx >= PRESET_COUNT ) {
			common->Printf( "dudePreset: index %d out of range (0..%d)\n", idx, PRESET_COUNT - 1 );
			return;
		}
		dude_preset.SetInteger( idx );
	} else {
		idx = dude_preset.GetInteger();
		if ( idx < 0 || idx >= PRESET_COUNT ) {
			// -1 = Custom / unset: nothing to apply
			return;
		}
	}
	ApplyEnhancementPreset( idx );
	// decision A: keep the base image-quality classification pinned at Ultra.
	com_machineSpec.SetInteger( 3 );
}

// DUDE: which preset the live cvars currently match, or -1 for "Custom". Exposed so
// the menu code (Session*.cpp) can push it into the GUI as "dude_preset" to drive
// the classic selector's highlight.
int Com_DetectDudePreset( void )
{
	return DetectEnhancementPreset();
}

static bool BeginTabChild( const char* name )
{
	bool ret = ImGui::BeginChild( name, ImVec2(0, 0), 0, ImGuiChildFlags_NavFlattened );
	float itemWidth = fminf( ImGui::GetWindowWidth() * 0.5f, ImGui::GetFontSize() * 20.0f );
	ImGui::PushItemWidth( itemWidth );
	return ret;
}

static ImVec2 settingsMenuDefaultSize;
static ImVec2 settingsMenuDefaultPos;

static bool d3settingsWinInitialized = false;

static void InitDhewm3SettingsMenu()
{
	InitBindingEntries();
	InitOptions( controlOptions, IM_ARRAYSIZE(controlOptions) );

	InitGraphicsMenu();
	InitAudioOptionsMenu();
	InitGameOptionsMenu();

	const ImGuiStyle& style = ImGui::GetStyle();
	float defaultWidth = ImGui::CalcTextSize( "Control BindingsControl OptionsGraphicsDebuggingAudio OptionsGame OptionsOther Options" ).x;
	defaultWidth += 2.0f * style.WindowPadding.x + 12.0f * style.FramePadding.x + 5.0f * style.ItemInnerSpacing.x;
	ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	settingsMenuDefaultSize.x = fminf( defaultWidth, displaySize.x * 0.8f );
	settingsMenuDefaultSize.y = displaySize.y * 0.95f;

	settingsMenuDefaultPos = displaySize * 0.025f;
	d3settingsWinInitialized = true;
}

// called from D3::ImGuiHooks::NewFrame() (if this window is enabled)
void Com_DrawDhewm3SettingsMenu()
{
	bool showSettingsWindow = true;

	if ( !d3settingsWinInitialized ) {
		// NOTE: InitDhewm3SettingsMenu() must be called after/by NewFrame(), because it calls
		//       ImGui::CalcTextSize() which needs a valid font texture. Especially after
		//       switching between windowed and fullscreen mode with Alt-Enter, calling it from
		//       Com_OpenCloseDhewm3SettingsMenu() caused problems (crashes)
		InitDhewm3SettingsMenu();
	}

	// to avoid people being too confused by the collapse window feature,
	// uncollapse it when it's being opened (so pressing F10 twice will restore your window)
	ImGui::SetNextWindowCollapsed( false, ImGuiCond_Appearing );
	// for some reason ImGui doesn't calculate a sane default window size when using
	// childwindows in the tabs and on first use the window is just the icons in the taskbar
	// so set a sane default size the first time it's opened (afterwards the size set by the
	// user is respected)
	ImGui::SetNextWindowSize( settingsMenuDefaultSize, ImGuiCond_FirstUseEver );
	// set a sane default pos first time each session (if the window somehow gets "lost" after
	// switching to a lower resolution, restarting DUDE will fix it)
	ImGui::SetNextWindowPos( settingsMenuDefaultPos, ImGuiCond_Once );

	ImGui::Begin("DUDE Settings", &showSettingsWindow);

	ImGuiTabBarFlags tab_bar_flags = ImGuiTabBarFlags_None;
	if (ImGui::BeginTabBar("SettingsTabBar", tab_bar_flags))
	{
		if ( ImGui::BeginTabItem("Control Bindings") ) {
			BeginTabChild("bindchild");
			DrawBindingsMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		} else {
			bindingsMenuAlreadyOpen = false;
		}
		if ( ImGui::BeginTabItem("Control Options") ) {
			BeginTabChild( "ctrlchild" );
			DrawOptions( controlOptions, IM_ARRAYSIZE(controlOptions) );
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		// Display / renderer / enhancement settings (the former "Video Options" tab was
		// folded in here). Always visible, but the non-vanilla enhancement controls are
		// greyed out unless the running backend supports them (see DrawGraphicsMenu).
		// The legacy ARB2 renderer stays faithful to vanilla Doom 3, so those effects
		// don't apply there.
		if ( ImGui::BeginTabItem("Graphics") )
		{
			BeginTabChild( "graphicschild" );
			DrawGraphicsMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		// developer live-tuning surface for shadow maps + emissive surfaces
		if ( ImGui::BeginTabItem("Debugging") )
		{
			BeginTabChild( "shadowdbgchild" );
			DrawShadowDebugMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Audio Options"))
		{
			ImGui::BeginChild( "audiochild" );
			DrawAudioOptionsMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Game Options"))
		{
			BeginTabChild( "gamechild" );
			DrawGameOptionsMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Other Options"))
		{
			BeginTabChild( "otherchild" );
			DrawOtherOptionsMenu();
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}

	if ( showStyleEditor ) {
		ImGui::Begin( "Dear ImGui Style Editor", &showStyleEditor );
		ImGui::ShowStyleEditor();
		ImGui::End();
	}

	ImGui::End();
	if (!showSettingsWindow) {
		D3::ImGuiHooks::CloseWindow( D3::ImGuiHooks::D3_ImGuiWin_Settings );
	}
}


// !! Don't call this function directly, always use                          !!
// !! D3::ImGuiHooks::OpenWindow( D3::ImGuiHooks::D3_ImGuiWin_Settings )     !!
// !! or D3::ImGuiHooks::CloseWindow( D3::ImGuiHooks::D3_ImGuiWin_Settings ) !!
// (unless you're implementing those two functions, they call this..)
void Com_OpenCloseDhewm3SettingsMenu( bool open )
{
	if ( open ) {
		if ( !sessLocal.IsMultiplayer() ) {
			// if we're in a SP game, pause the game.
			// g_stopTime is the best we have for this..
			// advantage of this, compared to the main menu with its settings menu:
			// gamma and brightness can be modified and the effect is visible in realtime,
			// but (at least in SP..) the player is still safe from monsters while doing this
			idCVar* stopTime = cvarSystem->Find( "g_stoptime" );
			if ( stopTime != nullptr ) {
				stopTime->SetBool( true );
			}
		}

		d3settingsWinInitialized = false; // make sure it's initialized in first frame
	} else {
		// unset g_stopTime (no matter if we're in MP now, maybe we weren't
		// when the menu was opened, just set it to false now to be sure)
		idCVar* stopTime = cvarSystem->Find( "g_stoptime" );
		if ( stopTime != nullptr ) {
			stopTime->SetBool( false );
		}
	}
}

void Com_Dhewm3Settings_f( const idCmdArgs &args )
{
	bool menuOpen = (D3::ImGuiHooks::GetOpenWindowsMask() & D3::ImGuiHooks::D3_ImGuiWin_Settings) != 0;
	if ( !menuOpen ) {
		D3::ImGuiHooks::OpenWindow( D3::ImGuiHooks::D3_ImGuiWin_Settings );
	} else {
		if ( ImGui::IsWindowFocused( ImGuiFocusedFlags_AnyWindow ) ) {
			// if the settings window is open and an ImGui window has focus,
			// close the settings window when "dudeSettings" (or its legacy alias) is executed
			D3::ImGuiHooks::CloseWindow( D3::ImGuiHooks::D3_ImGuiWin_Settings );
		} else {
			// if the settings window is open but no ImGui window has focus,
			// give focus to one of the ImGui windows.
			// useful to get the cursor back when ingame..
			ImGui::SetNextWindowFocus();
		}
	}
}

// ===========================================================================
// PBR material editor (docs/pbr-materials.md) — a floating ImGui window that
// edits the surface under the crosshair live and writes an override line.
// ===========================================================================

// combo order (friendly) mapped to pbrCategory_t values
static const char *pbrEditCatNames[] = {
	"none (pinned)", "metal", "metal_painted", "ceramic_sheen",
	"metal_rust", "stone", "skin", "eyes", "flesh"
};
static const int pbrEditCatEnum[] = {
	PBR_CAT_NONE, PBR_CAT_METAL, PBR_CAT_PAINTED, PBR_CAT_CERAMIC,
	PBR_CAT_RUST, PBR_CAT_STONE, PBR_CAT_SKIN, PBR_CAT_EYES, PBR_CAT_FLESH
};

static const idMaterial *pbrEditMat = NULL;	// material under edit (NULL = none picked)
static idStr pbrEditName;
static int   pbrEditCat = 0;
static float pbrEditMetal = 0.0f, pbrEditRough = 0.58f, pbrEditWet = 1.0f, pbrEditEnv = 1.0f;

static void PbrEditor_LoadFrom( const idMaterial *mat )
{
	pbrEditMat = mat;
	pbrEditName = mat ? mat->GetName() : "";
	if ( !mat ) {
		return;
	}
	pbrEditCat = mat->GetPbrCategory();
	if ( pbrEditCat > PBR_CAT_NONE && pbrEditCat < PBR_CAT_COUNT ) {
		// tracking a category: pre-fill all 4 sliders from that category's live preset,
		// so the editor shows the real current values (not the material's -1 placeholders)
		R_PbrCategoryDefaults( pbrEditCat, pbrEditMetal, pbrEditRough, pbrEditWet, pbrEditEnv );
	} else {
		// pinned / long-tail: the material's own explicit values, global roughness fallback
		pbrEditMetal = mat->GetPbrMetalness() >= 0.0f ? mat->GetPbrMetalness() : 0.0f;
		pbrEditRough = mat->GetPbrRoughness() >= 0.0f ? mat->GetPbrRoughness() : r_pbrRoughness.GetFloat();
		pbrEditWet   = mat->GetPbrWetness()   >= 0.0f ? mat->GetPbrWetness()   : 1.0f;
		pbrEditEnv   = mat->GetPbrEnv()       >= 0.0f ? mat->GetPbrEnv()       : 1.0f;
	}
}

// called by the editPbrMaterial command once it has traced a material
void Com_OpenPbrMaterialEditor( const idMaterial *mat )
{
	if ( mat ) {
		PbrEditor_LoadFrom( mat );
	}
	D3::ImGuiHooks::OpenWindow( D3::ImGuiHooks::D3_ImGuiWin_PbrEditor );
}

// one line: get/slider/set for a category cvar (live; archived so it persists)
static void PbrCvarSlider( const char *label, idCVar &cv, float mn, float mx )
{
	float v = cv.GetFloat();
	if ( ImGui::SliderFloat( label, &v, mn, mx, "%.2f" ) ) {
		cv.SetFloat( v );
	}
}

// "Material" tab: edit the surface under the crosshair (per-material override line)
static void PbrEditor_DrawMaterialTab()
{
	if ( pbrEditMat == NULL ) {
		ImGui::TextWrapped( "Aim at a surface and run \"editPbrMaterial\" (bind it, e.g. `bind p editPbrMaterial`) to load the material under the crosshair." );
		return;
	}
	ImGui::TextColored( ImVec4( 0.6f, 0.8f, 1.0f, 1.0f ), "%s", pbrEditName.c_str() );
	if ( !r_pbr.GetBool() ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.7f, 0.3f, 1.0f ),
			"r_pbr is 0 — enable PBR (Graphics tab) to see edits." );
	}
	ImGui::Separator();

	int comboIdx = 0;
	for ( int i = 0; i < IM_ARRAYSIZE( pbrEditCatEnum ); i++ ) {
		if ( pbrEditCatEnum[i] == pbrEditCat ) { comboIdx = i; }
	}
	if ( ImGui::Combo( "Category", &comboIdx, pbrEditCatNames, IM_ARRAYSIZE( pbrEditCatNames ) ) ) {
		pbrEditCat = pbrEditCatEnum[comboIdx];
		// re-attaching to a category re-fills the sliders from its preset (so you see and
		// start from its live values); selecting "none" keeps the current numbers as a pin
		if ( pbrEditCat > PBR_CAT_NONE && pbrEditCat < PBR_CAT_COUNT ) {
			R_PbrCategoryDefaults( pbrEditCat, pbrEditMetal, pbrEditRough, pbrEditWet, pbrEditEnv );
		}
	}

	// all four sliders are always live — no greying. Pre-filled from the category preset
	// when tracking; touching ANY of them forks this one material to a pinned custom.
	bool edited = false;
	edited |= ImGui::SliderFloat( "Metalness", &pbrEditMetal, 0.0f, 1.0f, "%.2f" );
	edited |= ImGui::SliderFloat( "Roughness", &pbrEditRough, 0.03f, 1.0f, "%.2f" );
	edited |= ImGui::SliderFloat( "Wetness (spec energy)", &pbrEditWet, 0.0f, 4.0f, "%.2f" );
	edited |= ImGui::SliderFloat( "Env glow (metal)", &pbrEditEnv, 0.0f, 4.0f, "%.2f" );

	// fork-on-edit: editing a category-tracked material detaches it to "none (pinned)".
	// The current values are already in the sliders, so the look is preserved — only this
	// material forks; the rest of the category is untouched. Re-attach via the dropdown.
	if ( edited && pbrEditCat != PBR_CAT_NONE ) {
		pbrEditCat = PBR_CAT_NONE;
	}
	const bool pinned = ( pbrEditCat == PBR_CAT_NONE );
	if ( pinned ) {
		ImGui::TextDisabled( "pinned (custom values) — pick a category above to re-attach and track it" );
	} else {
		ImGui::TextDisabled( "tracking '%s' — move any slider to fork this material to a custom pin", pbrEditCatNames[comboIdx] );
	}

	// tracking -> inherit (-1) so the material follows its category live; pinned -> all
	// four explicit (the fork snapshot)
	const float liveMetal = pinned ? pbrEditMetal : -1.0f;
	const float liveRough = pinned ? pbrEditRough : -1.0f;
	const float liveWet   = pinned ? pbrEditWet   : -1.0f;
	const float liveEnv   = pinned ? pbrEditEnv   : -1.0f;

	// live preview: push the current values straight onto the material
	const_cast<idMaterial *>( pbrEditMat )->SetPbrLive( liveMetal, liveRough, liveWet, liveEnv, pbrEditCat );

	ImGui::Separator();
	if ( ImGui::Button( "Save to pbr_overrides.cfg" ) ) {
		R_PbrWriteOverrideLine( pbrEditMat, liveMetal, liveRough, liveWet, liveEnv, pbrEditCat );
	}
	ImGui::SameLine();
	if ( ImGui::Button( "Re-pick" ) ) {
		const idMaterial *m = R_PbrPickCrosshairMaterial();
		if ( m ) {
			PbrEditor_LoadFrom( m );
		} else {
			D3::ImGuiHooks::ShowWarningOverlay( "No surface under the crosshair" );
		}
	}
	ImGui::SameLine();
	if ( ImGui::Button( "Revert" ) ) {
		const_cast<idMaterial *>( pbrEditMat )->ApplyPbrTable();	// restore file values
		PbrEditor_LoadFrom( pbrEditMat );
	}
}

// "Categories" tab: the shared per-category defaults. Every material tagged with a
// category reads these live at draw time, so a change moves the whole class at once;
// pinned 'none'/custom materials are unaffected. These are the same archived cvars
// as the Developer tab, so edits persist to the config.
static void PbrEditor_DrawCategoriesTab()
{
	ImGui::TextWrapped( "Shared defaults per category — change one and every material tagged with that category updates live. Pinned 'none'/custom materials keep their own values." );
	if ( !r_pbr.GetBool() ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.7f, 0.3f, 1.0f ), "r_pbr is 0 — enable PBR to see changes." );
	}

	ImGui::Spacing();
	PbrCategoryGrid();
	ImGui::Spacing();
	if ( ImGui::Button( "Save Category Defaults" ) ) {
		R_PbrWriteCategoryDefaults();
	}
	ImGui::SameLine();
	ImGui::TextDisabled( "-> pbr_overrides.cfg (@cat rows)" );

	ImGui::Spacing();
	PbrCvarSlider( "Metal Color Retention", r_pbrMetalDiffuse, 0.0f, 1.0f );
	ImGui::TextDisabled( "keeps the asset's painted color on metals (0 = physical kill, 1 = full)" );
}

// called from D3::ImGuiHooks::NewFrame() (if this window is enabled)
void Com_DrawPbrMaterialEditor()
{
	bool show = true;
	ImGui::SetNextWindowSize( ImVec2( 470.0f, 0.0f ), ImGuiCond_FirstUseEver );
	if ( ImGui::Begin( "PBR Material Editor", &show ) ) {
		if ( ImGui::BeginTabBar( "PbrEditorTabs" ) ) {
			if ( ImGui::BeginTabItem( "Material" ) ) {
				PbrEditor_DrawMaterialTab();
				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "Categories" ) ) {
				PbrEditor_DrawCategoriesTab();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
	if ( !show ) {
		if ( pbrEditMat ) {
			// discard unsaved live per-material edits on close (category cvars persist)
			const_cast<idMaterial *>( pbrEditMat )->ApplyPbrTable();
		}
		D3::ImGuiHooks::CloseWindow( D3::ImGuiHooks::D3_ImGuiWin_PbrEditor );
	}
}

void Com_EditPbrMaterial_f( const idCmdArgs &args )
{
	const idMaterial *m = R_PbrPickCrosshairMaterial();
	if ( m == NULL ) {
		common->Printf( "editPbrMaterial: no surface under the crosshair (aim at one and retry)\n" );
	}
	Com_OpenPbrMaterialEditor( m );
}

#else // IMGUI_DISABLE - just a stub function

#include "Common.h"

void Com_Dhewm3Settings_f( const idCmdArgs &args )
{
	common->Warning( "Dear ImGui is disabled in this build, so the DUDE settings menu is not available!" );
}

void Com_EditPbrMaterial_f( const idCmdArgs &args )
{
	common->Warning( "Dear ImGui is disabled in this build, so the PBR material editor is not available!" );
}

// DUDE: the enhancement presets live in the ImGui section; without it there is no
// preset table to apply. Stubs keep Common.cpp's command registration and the
// Session menu-var sync linking (e.g. the dedicated server, which always builds
// with IMGUI_DISABLE).
void Com_DudePreset_f( const idCmdArgs &args )
{
	common->Warning( "Dear ImGui is disabled in this build, so DUDE quality presets are not available!" );
}

int Com_DetectDudePreset( void )
{
	return -1;
}

#endif
