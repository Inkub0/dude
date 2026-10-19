#ifndef IMGUI_DISABLE

#include <algorithm> // std::sort - TODO: replace with something custom..

#include "sys/sys_sdl.h"

#define IMGUI_DEFINE_MATH_OPERATORS

#include "Common.h"

#include "idlib/LangDict.h"

#include "KeyInput.h"
#include "UsercmdGen.h" // key bindings
#include "FileSystem.h" // DUDE: default gamepad bindings cfg
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
extern idCVar com_interpolate;

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

// DUDE: reset a cvar we only have by name (no idCVar& in scope) to its registered default.
static void ResetNamedCvar( const char *name )
{
	idCVar *c = cvarSystem->Find( name );
	if ( c ) {
		c->Reset();
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


// DUDE: reset the gamepad bindings to the default Xbox-style layout that ships in
// base/gamepad.cfg (Doom 3) or base/gamepad-d3xp.cfg (Resurrection of Evil, the Lost
// Mission and mods built on d3xp - their weapon slots differ). The cfg is parsed here
// instead of exec'd so the change is synchronous and the bindings table updates at once;
// only JOY_* keys are touched, keyboard/mouse bindings stay as they are.
static bool IsD3XPBasedGame()
{
	const char* game = cvarSystem->GetCVarString( "fs_game" );
	const char* gameBase = cvarSystem->GetCVarString( "fs_game_base" );
	return idStr::Icmp( game, "d3xp" ) == 0 || idStr::Icmp( game, "d3le" ) == 0
		   || idStr::Icmp( gameBase, "d3xp" ) == 0;
}

static const char* DefaultGamepadCfgName()
{
	return IsD3XPBasedGame() ? "gamepad-d3xp.cfg" : "gamepad.cfg";
}

// returns the number of bindings applied, or -1 if the cfg couldn't be read
static int ApplyDefaultGamepadBindings()
{
	const char* cfgName = DefaultGamepadCfgName();
	char* buf = NULL;
	int len = fileSystem->ReadFile( cfgName, (void**)&buf, NULL );
	if ( len <= 0 || buf == NULL ) {
		return -1;
	}

	// clean slate, so old custom bindings don't linger on buttons the defaults leave free
	for ( int k = K_FIRST_JOY; k <= K_LAST_JOY; ++k ) {
		if ( k != K_JOY_BTN_START ) {
			idKeyInput::SetBinding( k, "" );
		}
	}

	int numBound = 0;
	idLexer lex( buf, len, cfgName, LEXFL_NOFATALERRORS | LEXFL_NOSTRINGCONCAT | LEXFL_NOSTRINGESCAPECHARS | LEXFL_ALLOWPATHNAMES );
	idToken tok, key, cmd;
	while ( lex.ReadToken( &tok ) ) {
		if ( tok.Icmp( "bind" ) == 0 && lex.ReadTokenOnLine( &key ) && lex.ReadTokenOnLine( &cmd ) ) {
			int keyNum = idKeyInput::StringToKeyNum( key.c_str() );
			if ( keyNum >= K_FIRST_JOY && keyNum <= K_LAST_JOY && keyNum != K_JOY_BTN_START ) {
				idKeyInput::SetBinding( keyNum, cmd.c_str() );
				++numBound;
			}
		}
		lex.SkipRestOfLine();
	}
	fileSystem->FreeFile( buf );
	return numBound;
}

static void DrawResetGamepadBindings()
{
	static int lastResult = -2; // -2: not applied yet this session
	static idStr lastCfg;
	static bool justOpened = false;
	const char* popupTitle = "Reset Gamepad Bindings###resetGamepadBinds";

	if ( ImGui::Button( "Reset Gamepad Bindings to Xbox-style Defaults" ) ) {
		ImGui::OpenPopup( popupTitle );
		justOpened = true;
	}
	{
		// names of the face buttons as the current pad calls them (A/X/Back on xbox)
		idStr btnSouth = Sys_GetLocalizedJoyKeyName( K_JOY_BTN_SOUTH );
		idStr btnWest = Sys_GetLocalizedJoyKeyName( K_JOY_BTN_WEST );
		idStr btnBack = Sys_GetLocalizedJoyKeyName( K_JOY_BTN_BACK );
		AddTooltip( idStr::Format( "Replaces all gamepad bindings with the default layout from %s:\n"
								   "Left Stick: Move (click: Run)    Right Stick: Look (click: Crouch)\n"
								   "%s: Jump    %s: Reload    %s: PDA\n"
								   "Left/Right Shoulder: Previous/Next Weapon    D-Pad: Weapon shortcuts\n"
								   "Left Trigger: Flashlight    Right Trigger: Attack",
								   DefaultGamepadCfgName(), btnSouth.c_str(), btnWest.c_str(), btnBack.c_str() ).c_str() );
	}

	if ( ImGui::BeginPopupModal( popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize ) )
	{
		ImGui::Text( "Replace ALL current gamepad bindings with the defaults from %s ?", DefaultGamepadCfgName() );
		ImGui::NewLine();
		ImGui::Text( "Press Enter (or %s) to confirm, or\nEscape (or %s) to cancel.",
					 GetGamepadBindNowButtonName(), GetGamepadCancelButtonNames() );
		ImGui::NewLine();

		// center the Ok and Cancel buttons
		float dialogButtonWidth = CalcDialogButtonWidth();
		float spaceBetweenButtons = ImGui::GetFontSize();
		float buttonOffset = (ImGui::GetWindowWidth() - 2.0f*dialogButtonWidth - spaceBetweenButtons) * 0.5f;
		ImGui::SetCursorPosX( buttonOffset );

		// not on the frame the popup opened: the button press that opened it would confirm at once
		bool confirmedByKey = !justOpened && !ImGui::IsAnyItemFocused() && IsConfirmKeyPressed();
		justOpened = false;
		if ( ImGui::Button( "Ok", ImVec2(dialogButtonWidth, 0) ) || confirmedByKey ) {
			lastCfg = DefaultGamepadCfgName();
			lastResult = ApplyDefaultGamepadBindings();
			UpdateKeyBindingsInEntries();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine( 0.0f, spaceBetweenButtons );
		if ( ImGui::Button( "Cancel", ImVec2(dialogButtonWidth, 0) ) || IsCancelKeyPressed() ) {
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if ( lastResult == -1 ) {
		ImGui::SameLine();
		ImGui::TextColored( ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Couldn't read %s!", lastCfg.c_str() );
	} else if ( lastResult >= 0 ) {
		ImGui::SameLine();
		ImGui::TextDisabled( "Applied %d bindings from %s", lastResult, lastCfg.c_str() );
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


		DrawResetGamepadBindings();

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

// DUDE: cvar-bound widget helpers. The hand-drawn tuning groups repeated the same
// read / widget / write-on-change / tooltip boilerplate ~180 times; these collapse it to one
// call each. Behaviour is byte-for-byte the longhand: the widget writes the cvar only when it
// reports a change (ImGui's return value), and the tooltip is the same AddTooltip. Reset buttons
// stay as their own lines next to the call (see idCVar::Reset / ResetNamedCvar). Sliders that
// need a value clamp, non-default flags, or discrete stops keep their longhand form.
// each returns the cvar's post-edit value, so a caller that gates a following BeginDisabled /
// conditional on the control can capture it (e.g. `bool on = CvarCheckbox( ... );`).
static bool CvarCheckbox( const char* label, idCVar& cv, const char* tip = nullptr )
{
	bool b = cv.GetBool();
	if ( ImGui::Checkbox( label, &b ) ) {
		cv.SetBool( b );
	}
	if ( tip ) { AddTooltip( tip ); }
	return b;
}

static float CvarSliderF( const char* label, idCVar& cv, float mn, float mx, const char* fmt, const char* tip = nullptr )
{
	float f = cv.GetFloat();
	if ( ImGui::SliderFloat( label, &f, mn, mx, fmt ) ) {
		cv.SetFloat( f );
	}
	if ( tip ) { AddTooltip( tip ); }
	return f;
}

static int CvarSliderI( const char* label, idCVar& cv, int mn, int mx, const char* tip = nullptr )
{
	int i = cv.GetInteger();
	if ( ImGui::SliderInt( label, &i, mn, mx ) ) {
		cv.SetInteger( i );
	}
	if ( tip ) { AddTooltip( tip ); }
	return i;
}

// DUDE: a float cvar presented as a discrete "stops" slider (the resolution-scale controls).
// Snaps the current value to the nearest stop for display, and writes the chosen stop on change.
// `labels` is parallel to `stops` (shown as the slider's value text; NoInput so only the stops pick).
static void CvarStopsSliderF( const char* label, idCVar& cv, const float* stops, const char* const* labels, int numStops, const char* tip = nullptr )
{
	const float cur = cv.GetFloat();
	int idx = 0;
	float best = 1e9f;
	for ( int i = 0; i < numStops; i++ ) {
		const float d = idMath::Fabs( cur - stops[i] );
		if ( d < best ) { best = d; idx = i; }
	}
	if ( ImGui::SliderInt( label, &idx, 0, numStops - 1, labels[idx], ImGuiSliderFlags_NoInput ) ) {
		idx = idx < 0 ? 0 : ( idx >= numStops ? numStops - 1 : idx );
		cv.SetFloat( stops[idx] );
	}
	if ( tip ) { AddTooltip( tip ); }
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
	CVarOption("in_gamepadDevice", [](idCVar& cvar) {
			// DUDE: which connected pad feeds the game and this menu (empty = all merged)
			const char* cur = cvar.GetString();
			const int numPads = Sys_GetNumGamepads();
			int sel = -1; // -1 = all
			idStr preview;
			if ( cur[0] == '\0' ) {
				preview = ( numPads > 1 ) ? idStr::Format( "All connected gamepads (%d)", numPads )
										  : idStr( "All connected gamepads" );
			} else {
				for ( int i = 0; i < numPads; ++i ) {
					if ( idStr::Icmp( Sys_GetGamepadGUID( i ), cur ) == 0 || idStr::Icmp( Sys_GetGamepadName( i ), cur ) == 0 ) {
						sel = i;
						break;
					}
				}
				preview = ( sel >= 0 ) ? idStr( Sys_GetGamepadName( sel ) )
									   : idStr::Format( "Not connected: %s (using all)", cur );
			}
			if ( ImGui::BeginCombo( "Gamepad Device", preview.c_str() ) ) {
				if ( ImGui::Selectable( "All connected gamepads", sel == -1 ) ) {
					cvar.SetString( "" );
				}
				for ( int i = 0; i < numPads; ++i ) {
					const char* path = Sys_GetGamepadPath( i );
					idStr label = idStr::Format( "%s%s%s%s###pad%d", Sys_GetGamepadName( i ),
												 path[0] ? " (" : "", path, path[0] ? ")" : "", i );
					if ( ImGui::Selectable( label.c_str(), sel == i ) ) {
						cvar.SetString( Sys_GetGamepadGUID( i ) );
					}
					AddTooltip( idStr::Format( "GUID: %s", Sys_GetGamepadGUID( i ) ).c_str() );
				}
				if ( numPads == 0 ) {
					ImGui::TextDisabled( "(no gamepad connected)" );
				}
				ImGui::EndCombo();
			}
			AddCVarOptionTooltips( cvar, "Which gamepad drives the game and this menu.\n"
										 "Useful when two show up, e.g. a pad's wireless dongle and its USB cable:\n"
										 "pick the one you actually hold. 'All' merges the input of every connected pad." );
		}),
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
	// DUDE skin / flesh subsurface scattering (docs/skin-shading.md). A wrapped diffuse on the
	// classifier's skin/flesh materials that warms the lighting terminator so faces and Hell flesh
	// stop reading as plastic under the GGX lobe. Diffuse-only, so it works on every shading model
	// (not just PBR). Only the on/off lives here -- strength and reach are live-tuning knobs and
	// sit in the Debugging tab with the rest of them (DrawDbgGroup_SkinSSS).
	CVarOption( "r_skinShading", []( idCVar& cvar ) {
		// r_skinShading doubles as strength AND enable, so the master checkbox toggles between 0 and
		// the last non-zero strength (remembered so an off/on round-trip restores the amount; falls
		// back to 0.6, the preset value).
		static float lastOn = 0.6f;
		float cur = cvar.GetFloat();
		if ( cur > 0.0f ) {
			lastOn = cur;
		}
		bool on = cur > 0.0f;
		if ( ImGui::Checkbox( "Skin Subsurface Scattering (SSS)", &on ) ) {
			cvar.SetFloat( on ? ( lastOn > 0.0f ? lastOn : 0.6f ) : 0.0f );
		}
		const char* descr = "Faces and Hell flesh scatter light under the skin, so their shading terminator glows\n"
			"a soft red instead of cutting off hard and plastic-looking. Keyed off the material\n"
			"classifier's skin/flesh tags (no per-map setup) and works on every tier, not just PBR.\n"
			"Purely a look; strength and reach are tuned in the Debugging tab.";
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
		int sel = idMath::ClampInt( 0, 5, cvar.GetInteger() );
		if ( ImGui::Combo( "Tonemap", &sel, "Off (faithful)\0Reinhard\0ACES\0AgX\0Khronos PBR Neutral\0DUDE\0" ) ) {
			cvar.SetInteger( sel );
		}
		const char* descr = "Compress the HDR scene's bright highlights into the display range with a filmic\n"
			"curve, instead of hard-clipping them to white. Off reproduces the vanilla resolve\n"
			"exactly; ACES is the game-standard look, AgX and PBR Neutral shift colour the least.\n"
			"DUDE is our signature curve: it leaves blacks and midtones exactly as authored and\n"
			"only rolls off the >1.0 highlights (hue-preserving), so it suits Doom 3's LDR art.\n"
			"Exposure (in the Display group) feeds the curve; it does nothing while Tonemap is Off.";
		AddCVarOptionTooltips( cvar, descr );

		// Exposure and HDR Gamma moved to the Display group (they are the HDR counterparts of
		// Brightness / Gamma and now sit right under them). This block stays tonemap-gated: the DUDE
		// dials, emissive overbright and eye-adaptation below only do anything with a curve on.
		ImGui::BeginDisabled( sel == 0 );

		// DUDE-curve dials (r_hdrTonemap 5 only): the knee sets where highlights start rolling
		// off (blacks/midtones below it stay untouched), the desaturation gives bright cores a
		// white-hot centre instead of a saturated blob. Shown only when DUDE is the active curve.
		if ( sel == 5 ) {
			float knee = cvarSystem->GetCVarFloat( "r_hdrDudeKnee" );
			if ( ImGui::SliderFloat( "DUDE Knee", &knee, 0.05f, 0.95f, "%.2f" ) ) {
				cvarSystem->SetCVarFloat( "r_hdrDudeKnee", knee );
			}
			AddTooltip( "r_hdrDudeKnee: where the DUDE curve begins rolling off highlights. Below it the scene\n"
				"is untouched (faithful blacks + midtones); above it highlights compress toward white.\n"
				"Lower = start sooner and keep more highlight range; higher = touch only the very brightest." );
			float ddesat = cvarSystem->GetCVarFloat( "r_hdrDudeDesat" );
			if ( ImGui::SliderFloat( "DUDE Highlight White-Hot", &ddesat, 0.0f, 4.0f, "%.2f" ) ) {
				cvarSystem->SetCVarFloat( "r_hdrDudeDesat", ddesat );
			}
			AddTooltip( "r_hdrDudeDesat: how strongly bright cores (fireballs, lava, muzzle flash) fade toward\n"
				"white as they compress, so they read as detail instead of a flat saturated blob.\n"
				"0 = pure hue-preserving (the red-blob look); raise it to give hot cores a white centre." );
			float dtint = cvarSystem->GetCVarFloat( "r_hdrDudeTint" );
			if ( ImGui::SliderFloat( "DUDE White-Hot Tint", &dtint, 0.0f, 1.0f, "%.2f" ) ) {
				cvarSystem->SetCVarFloat( "r_hdrDudeTint", dtint );
			}
			AddTooltip( "r_hdrDudeTint: what colour the white-hot centre is. 0 = neutral white, which can make a\n"
				"saturated lava/fire core read pink on its way to white; 1 = keep the highlight's own hue.\n"
				"~0.35 keeps molten cores a warm yellow-white. Raise it if bright colours look washed or pink." );
		}

		// Emissive overbright, same tonemap-gated block. Pushes additive self-illum
		// surfaces above 1.0 so the curve rolls them off as real highlights; like
		// exposure it only acts with a curve on, so it greys out when Tonemap is Off.
		float overbright = cvarSystem->GetCVarFloat( "r_hdrOverbright" );
		if ( ImGui::SliderFloat( "Emissive Overbright", &overbright, 1.0f, 8.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrOverbright", overbright );
		}
		AddTooltip( "r_hdrOverbright: multiply additive self-illum surfaces (lamps, screens, fire, glares)\n"
			"so they exceed 1.0 and the tonemap curve treats them as real highlights (and eye-adaptation\n"
			"has bright anchors). 1 = off; ~1.60 default (tuned for DUDE). When active it auto-disables the\n"
			"legacy light-flare/glare haze (r_flareSize). No effect while Tonemap is Off." );
		float obsat = cvarSystem->GetCVarFloat( "r_hdrOverbrightSat" );
		if ( ImGui::SliderFloat( "Overbright Saturation", &obsat, 1.0f, 3.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrOverbrightSat", obsat );
		}
		AddTooltip( "r_hdrOverbrightSat: pre-saturate coloured emissive (fire, lava) before the tonemap so\n"
			"it doesn't wash to white when overbright pushes it very bright. White lights are unaffected.\n"
			"1 = off. Only acts on overbright additive stages." );

		// Eye adaptation (Phase B1): auto-exposure driven by the scene's average luminance. The
		// Exposure slider (Display group) becomes the mid-gray target; brighter scenes expose down over time.
		ImGui::Spacing();
		bool eye = cvarSystem->GetCVarBool( "r_hdrEyeAdaptation" );
		if ( ImGui::Checkbox( "Eye Adaptation (auto-exposure)", &eye ) ) {
			cvarSystem->SetCVarBool( "r_hdrEyeAdaptation", eye );
		}
		AddTooltip( "r_hdrEyeAdaptation: the scene's average luminance drives the tonemap exposure over\n"
			"time (dark rooms brighten, bright scenes dim). Needs a tonemap curve; opengl3 / Vulkan.\n"
			"Fine-tuning knobs are in the Debug tab -> Eye Adaptation. Off = the static Exposure." );
		ImGui::EndDisabled();

		// Bloom (Phase C): glow bled from bright pixels, added before the tonemap. Needs r_hdr and
		// works with any curve, so it sits outside the tonemap-off greying (but inside r_hdr).
		ImGui::Spacing();
		float bloom = cvarSystem->GetCVarFloat( "r_hdrBloom" );
		if ( ImGui::SliderFloat( "Bloom", &bloom, 0.0f, 2.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrBloom", bloom );
		}
		AddTooltip( "r_hdrBloom: glow bled from bright pixels, added before the tonemap — the punch the\n"
			"curve otherwise rolls off. 0 = off. opengl3 / Vulkan." );
		ImGui::BeginDisabled( bloom <= 0.0f );
		float bthr = cvarSystem->GetCVarFloat( "r_hdrBloomThreshold" );
		if ( ImGui::SliderFloat( "Bloom Threshold", &bthr, 0.0f, 8.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrBloomThreshold", bthr );
		}
		AddTooltip( "r_hdrBloomThreshold: brightness a pixel must exceed to bloom. Lower it to catch\n"
			"fire/lava directly (can replace the emissive overbright); raise to bloom only the\n"
			"brightest highlights." );
		ImGui::EndDisabled();

		ImGui::EndDisabled();
	} ),
	// Film Grain: its own clearly-labelled block. It used to trail the Eye-Adaptation sliders
	// (which live inside the HDR tonemap group just above) with no heading, so it read as part of
	// Eye Adaptation. Now a switch + intensity + size, bound to r_postFilmGrain / r_postFilmGrainSize.
	CVarOption( "r_postFilmGrain", []( idCVar& cvar ) {
		ImGui::SeparatorText( "Film Grain" );
		static float lastOn = 0.05f;					// remember the on-intensity so the switch can restore it
		if ( cvar.GetFloat() > 0.0f ) { lastOn = cvar.GetFloat(); }
		bool on = cvar.GetFloat() > 0.0f;
		if ( ImGui::Checkbox( "Enable Film Grain##on", &on ) ) {
			cvar.SetFloat( on ? ( lastOn > 0.0f ? lastOn : 0.05f ) : 0.0f );
		}
		AddCVarOptionTooltips( cvar, "Animated filmic grain over the finished image. Off = clean; on adds a\n"
			"subtle sensor-like noise that masks colour banding and gives a filmic texture.\n"
			"The switch remembers your last intensity; setting Intensity to 0 is also off." );
		ImGui::BeginDisabled( cvar.GetFloat() <= 0.0f );
		float inten = cvar.GetFloat();
		if ( ImGui::SliderFloat( "Intensity##grain", &inten, 0.0f, 0.25f, "%.2f" ) ) { cvar.SetFloat( inten ); }
		AddCVarOptionTooltips( cvar );
		float gsz = r_postFilmGrainSize.GetFloat();
		if ( ImGui::SliderFloat( "Size (px)##grain", &gsz, 1.0f, 4.0f, "%.2f" ) ) { r_postFilmGrainSize.SetFloat( gsz ); }
		AddCVarOptionTooltips( r_postFilmGrainSize );
		ImGui::EndDisabled();
	} ),
	CVarOption( "r_postChromaticAberration", "Chromatic Aberration", OT_FLOAT, 0.0f, 0.5f ),
	// Weapon-reload depth of field: the on/off toggle (tuning sliders are in Debug -> Depth of Field).
	CVarOption( "r_dof", "Weapon-Reload Depth of Field", OT_BOOL ),
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
	                       "divisor of it (e.g. 72 on a 144 Hz panel). In VRR mode this is managed for you "
	                       "(paced just below refresh); a lower value here still wins." );

	// Fullscreen Desktop
	CvarCheckbox( "Fullscreen Desktop (borderless at desktop resolution)", r_fullscreenDesktop, "r_fullscreenDesktop" );
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
	// Combo indices: 0 Disable, 1 Enable, 2 Adaptive, 3 VRR (variable refresh rate).
	// VRR rides on vsync-on (FIFO) plus the frame limiter pacing just below refresh,
	// so selecting it forces r_swapInterval 1 and flips r_vrr on.
	int curVsync;
	if ( r_vrr.GetBool() ) {
		curVsync = 3;
	} else {
		curVsync = idMath::ClampInt( -1, 1, r_swapInterval.GetInteger() );
		if ( curVsync == -1 ) {
			curVsync = 2;
		}
	}
	if ( ImGui::Combo( "Vertical Sync", &curVsync, "Disable VSync\0Enable VSync\0Adaptive VSync\0VRR (G-SYNC / FreeSync)\0" ) ) {
		const bool wantVrr = ( curVsync == 3 );
		const int interval = wantVrr ? 1 : ( curVsync == 2 ? -1 : curVsync );
		if ( GLimp_SetSwapInterval( interval ) ) {
			r_swapInterval.SetInteger( interval );
			// this was just set with GLimp_SetSwapInterval(), no reason to set it again in R_CheckCvars()
			r_swapInterval.ClearModified();
			r_vrr.SetBool( wantVrr );
		} else {
			D3::ImGuiHooks::ShowWarningOverlay( "Setting VSync (GL SwapInterval) failed, maybe try another mode" );
		}
	} else {
		AddTooltip( "r_swapInterval" );
	}
	AddDescrTooltip( "VRR keeps VSync on and auto-paces the frame rate just below your refresh so it stays "
	                 "inside the G-SYNC / FreeSync / Adaptive-Sync window. Enable VRR in your GPU driver or "
	                 "compositor first. Note: Adaptive VSync isn't supported on all GPUs/drivers." );
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

// Ray tracing is an ORTHOGONAL axis to the raster quality tier above, not the top rung of it.
// Every raster tier (Potato..Nightmare) runs on any GL3.3 card with RTX off; on a Vulkan +
// ray-query GPU the player can switch RTX on, which layers a tier-scaled RT feature set on top
// via rtAugment[] below (High = RT shadows, Ultra = + RTAO, Nightmare = the full ex-"Ultra
// Nightmare" RT set). "Nightmare + RTX On" carries the whole RT feature set the old
// PRESET_ULTRA_NIGHTMARE had, and the status line still shows that name; the two purely-raster
// knobs that tier used to also bump (anisotropy 4->6, SSAO res 0.8->1.0) now stay at Nightmare's
// values, since RTX is orthogonal and must not change the raster look. PATHTRACED is reserved
// for H7 ("Path Tracing", dude_rtx 2; docs/rtx-restir-di.md).
enum RtMode {
	RTX_OFF = 0,
	RTX_ON,				// hybrid: RT augments the chosen raster tier
	RTX_PATHTRACED,		// "Path Tracing" (dude_rtx 2): the tier's RT augment + ReSTIR DI lighting the scene (docs/rtx-restir-di.md)
	RTX_COUNT
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
	// PN-triangle character smoothing from High up. tessDisplace is 0.12 (authored relief)
	// on every tier except Potato (which stays 0 = pure silhouette smoothing); it is inert
	// on Low/Medium (tessellation off there) and applies from High up, or wherever
	// tessellation is hand-enabled.
	bool  tessellation;             // r_tessellation
	float tessDisplace;             // r_tessDisplace
	// DUDE parallax occlusion mapping (appended, see note above; GL3 + Vulkan): world-surface
	// relief from Medium up. Self-shadowing (the second, half-step march) is the cost swing, so
	// it's off on Medium and full from High up, EXCEPT Path Tracing, which eases it to 0.5:
	// there RTAO already occludes the relief's crevices, so full POM self-shadow double-darkens.
	// parallaxShadow is inert where parallax is off.
	bool  parallax;                 // r_parallax
	float parallaxShadow;           // r_parallaxShadow
	// DUDE GPU MD5 skinning (appended, see note above; Vulkan-only, inert on GL3). ON from Medium up —
	// it offloads the per-frame skin DRAW to the GPU (the CPU position skin still runs, so monster hit
	// surfaces are unaffected), and the seam-weld work made it visually clean under tessellation. Its
	// option-B TBN is a small fidelity divergence from stock's per-frame re-derive, so Potato/Low keep it
	// OFF to stay a faithful id-render (the faithful floor); every preset writing it explicitly keeps that
	// floor bulletproof even if it was hand-toggled before switching presets. The cvar's own default is on
	// (Vulkan), so a fresh boot with no preset applied still gets it.
	bool  gpuSkin;                  // r_gpuSkinning
	// SSAO depth-mip acceleration (appended, see note above). On for every tier that runs SSAO
	// (Medium/High/Ultra/Nightmare) — the ~25% AO speedup for near-invisible halos at the
	// archived bias 0.1 / cap 2 defaults. Inert on Potato/Low (SSAO off). The bias/cap knobs
	// stay at their archived cvar defaults across tiers.
	bool  ssaoDepthMip;             // r_ssaoDepthMip
	// Per-object motion vectors (appended, see note above; Vulkan-only, inert elsewhere).
	// On for Ultra/Nightmare only: their SSR temporal accumulation is the consumer that
	// visibly benefits (moving objects stop dragging reflections), and they already fund
	// the normal prepass (ssaoNormalBuffer) the velocity MRT rides on. Medium's temporal
	// SSAO reconstructs normals from depth (no prepass), so MV there would add a whole
	// opaque pass on the budget tier for a marginal win. FSR2 (r_fsr) implies the
	// velocity buffer regardless of this flag.
	bool  motionVectors;            // r_motionVectors
	// FSR2 Native-AA (appended, see note above; R1/E, Vulkan-only, inert elsewhere).
	// Nightmare only: the temporal resolve that kills D3's specular/normal-map shimmer,
	// ~1.7 ms GPU at 1440p — exactly what the top tier is for. rhiAA deliberately STAYS
	// SMAA in the row (deviation from the plan's rhiAA=0): the HDR resolve already
	// bypasses FXAA/SMAA while FSR2 runs (no stacking), and keeping it gives the GL3
	// backend — where r_fsr is inert — its SMAA fallback on the same preset.
	bool  fsr;                      // r_fsr
	// Split-cache movers' cube resolution drop (appended, see note above). The dynamic
	// (movers-only) layer re-renders every frame, so its resolution is pure recurring
	// fill cost; 1 = half res reads as penumbra on moving shadows (user-verified,
	// docs/shadow-research.md 2026-08 recency check). Nightmare alone keeps full res —
	// the no-compromise tier. Inert on Potato/Low (shadow maps off), carried at 1 there.
	int   splitDynDrop;             // r_shadowMapSplitDynDrop
	// Ray-traced shadows (appended, see note above; Vulkan + RT hardware only, INERT on GL3
	// and on non-RT GPUs — the cvars gate on SupportsRayQuery). Path Tracing only: RT sun
	// shadows (exact at any distance, no map aliasing) + RT shadows for moving point lights
	// (deletes the un-cacheable per-frame cube re-render). Every lower tier carries 0, so a
	// non-RT machine sees Path Tracing == Nightmare visually rather than a broken tier.
	bool  rtSunShadows;             // r_rtSunShadows
	bool  rtMovingLights;           // r_rtMovingLights
	// RT reflections (appended, see note above; VK + RT hardware only, inert on GL3/non-RT). Ultra
	// Nightmare only: trace reflections off the persistent world BLAS + animated-monster BLAS so
	// reflective floors/metal show the real room + monsters (including off-screen), replacing the SSR
	// composite at that tier (docs/rtx-reflections.md RR7). The TLAS comes from rtSunShadows (which
	// auto-builds the per-surface world BLAS); the monster BLAS comes from r_rtAnimBlas, which is now
	// default-ON and inert without a TLAS, so it is NOT preset-controlled (no tier can force it off and
	// silently break monster reflections).
	bool  rtReflections;            // r_rtReflections
	// SSAO temporal-funded sample budget (appended, see note above). When on, the AO pass
	// runs a fixed 4 slices x 6 steps with temporal accumulation forced on (overriding the
	// slice/step/temporal columns) — cheaper than brute-force sampling for a look the user
	// judged only slightly softer (2026-09-16). ON for every SSAO tier except the top two:
	// Nightmare / Path Tracing stay brute-force (5x10, temporal off), the no-compromise
	// look. Potato/Low carry ON per the cheap-sub-params convention (inert, SSAO off).
	bool  ssaoTemporalTrade;        // r_ssaoTemporalTrade
	// Render interpolation (appended, see note above). The engine's fixed 60Hz game sim
	// caps rendering at 60fps unless the view interpolates between tics; every enhanced
	// tier turns it ON so high-refresh displays actually get high fps. Potato keeps the
	// stock 60Hz-locked behaviour — the faithful floor (user decision 2026-09-17).
	bool  interpolate;              // com_interpolate
	// Ray-traced AO (appended, see note above; VK + RT hardware only, inert elsewhere).
	// Path Tracing only: replaces the screen-space GTAO producer with one traced
	// hemisphere ray per pixel + NRD ReBLUR — the tier's "drop screen-space AO for RT"
	// step (docs/rtx-rtao.md). Needs r_ssao (master AO switch, on at this tier) + the
	// velocity buffer for the denoiser (r_fsr, on here). Every lower tier carries 0, so a
	// non-RT machine sees UN == Nightmare AO rather than a broken tier.
	bool  rtao;                     // r_rtao
	// RT shadow blur (appended; VK + RT hardware only, inert elsewhere): softens the two hard
	// ray-traced shadow routes above in screen space (docs/rtx-shadow-blur.md). Path Tracing
	// only - it needs those routes (on at this tier only) and changes the look of shadows.
	// Measured ~+0.9 ms per full-screen ray-served light at 1440p on a 3080 Ti.
	bool  rtShadowBlur;             // r_rtShadowBlur
	// RT shadows for every shadow-casting light (appended; VK + RT hardware only, inert
	// elsewhere): all lights take the inline hard-ray route and no shadow map renders at all
	// (docs/rtx-shadow-blur.md). Path Tracing only. Measured +0.25 ms GPU in a busy Mars City
	// scene on a 3080 Ti @1440p; with rtShadowBlur softening all of them, +5.2 ms (about half of
	// that back with r_rtShadowBlurStagger, which stays a manual opt-in).
	bool  rtAllLights;              // r_rtAllLights
	// Distance-tiered refresh of the blurred shadow masks (appended; only meaningful with
	// rtShadowBlur): lights beyond r_rtShadowBlurStaggerNear / Far refresh every 2nd / 3rd frame,
	// reprojected in between. Path Tracing only - it is what makes blurring EVERY light
	// affordable there (user-measured 15.6 -> 12.8 ms, no visible change). The preset owns the
	// on/off only; the two distances are free tuning knobs (Debugging -> RT Shadows).
	bool  rtBlurStagger;            // r_rtShadowBlurStagger
	// Volumetric lighting (appended; docs/volumetric-lighting.md): the air scatters the scene's real
	// lights - cones, shafts, a visible flashlight beam. Path Tracing only: it needs the RT scene that
	// tier already builds (visibility rays) and HDR. The preset owns the on/off only; which lights
	// scatter, the haze density and the other r_vol* knobs are free tuning (Graphics -> Lighting).
	bool  volumetrics;              // r_volumetrics
	// RTAO resolution (appended; docs/rtx-rtao.md): the fraction of the screen RTAO runs its rays
	// and the NRD denoiser at (the occlusion is upscaled). Only meaningful where rtao is on, so
	// only the Path Tracing tier carries < 1.0; every other tier is 1.0 (full res).
	float rtaoResScale;            // r_rtaoResScale
	// Skin / flesh subsurface scattering (appended, see note above; GL3 + Vulkan, all tiers). A
	// wrapped diffuse that warms the lighting terminator on classifier-tagged skin/flesh so faces
	// and Hell flesh stop reading as plastic. Cheap and diffuse-only, so it rides every shading
	// model; ON from Low up. Potato alone keeps it 0 — the faithful id-render floor (it changes the
	// diffuse falloff, a deliberate deviation from stock Lambert). The reach knob stays the cvar
	// default across tiers; only the strength is preset-owned.
	float skinShading;             // r_skinShading
	// RT Global Illumination (appended; docs/rtx-rtgi.md H6): Path Tracing only - one traced bounce of
	// indirect diffuse, denoised, which REPLACES RTAO there (so the tier also carries rtao=false; the single
	// NRD instance does GI instead, and screen-space GTAO fills the scalar AO reflections use). Needs the RT
	// scene + velocity buffer (r_fsr, on here). Every lower tier carries false. Intensity / ambient-keep /
	// resolution are free tuning knobs (Graphics -> the RT section), like volumetrics.
	bool  rtgi;                     // r_rtgi
	// HDR display gamma (appended, see note above; a live shader uniform, effective wherever the
	// DUDE tonemap curve is on — Medium up). Path Tracing eases it to 1.0 (neutral) because its
	// one traced GI bounce lifts the scene's overall radiance; every other tier carries the cvar
	// default (1.20). Exposure is deliberately NOT tiered — it is a plain cvar default (1.3) across
	// every tier. Inert on Potato/Low (tonemap off), carried at the default there.
	float hdrGamma;                 // r_hdrGamma
	// Max texture anisotropy (appended, see note above; image_anisotropy). Works on every backend —
	// GL/GL3 set it per texture object, VK bakes it into the sampler (RefreshAnisotropySamplers). The
	// requested level is clamped to the device max at apply time, so e.g. 6 on a 4x card becomes 4.
	// Potato/Low 1 (the faithful, stock-Doom3 floor); Medium 2; High 4; Ultra 8; Nightmare 16. Clamped
	// to the device max at apply time, so a level above the card's cap quietly becomes the cap.
	int   anisotropy;               // image_anisotropy
};

// Potato/Low keep the enhancements off but carry the cheap Medium sub-params, so
// flipping a feature on by hand from those tiers stays affordable and detection
// stays unambiguous. High == shipped defaults plus the pipeline tiers (HDR + PBR;
// see anchor note above — the pipeline columns deliberately exceed the cvar
// defaults, which keep every enhancement off).
static const EnhancementPreset enhancementPresets[PRESET_COUNT] = {
	//                soft   smoke  emiss  ssao   shadow  aoRes aoSl aoSt aoNB   aoBN   smSz  smPt  pcf ptLim emLim grain  chrom  refl  shd sScl  sExp   szScl szRad   occl   hdr    pbr    ssr    ssrRes  grainSz aa aoRad   aoTmp   tess   tessDsp  parlx  parlxSh gpuSkn dMip   mv     fsr    dynDrop rtSun  rtMov  rtRefl aoTrade interp rtao   rtBlur rtAll  stagger  volum  rtaoRes  skin   rtgi  hdrGam aniso
	{ "Potato",       false, false, false, false, false,  0.5f, 3,   1,   false, true,  512,  512,  5,  16,   16,   0.0f,  0.0f,  1.0f, 0,  1.0f, 62.0f, true, 380.0f, false, false, false, false, 1.0f,   1.5f,   0, 48.0f,  false,  false, 0.0f, false, 0.0f, false, false, false, false, 1,     false, false, false, true,  false, false, false, false, false, false, 1.0f, 0.0f, false, 1.20f, 1 },
	{ "Low",          true,  false, false, false, false,  0.5f, 3,   1,   false, true,  512,  512,  5,  16,   16,   0.05f, 0.0f,  1.0f, 1,  1.2f, 42.0f, true, 380.0f, true,  false, false, false, 1.0f,   1.5f,   2, 48.0f,  false,  false, 0.12f, false, 0.0f, false, false, false, false, 1,     false, false, false, true,  true , false, false, false, false, false, 1.0f, 0.6f, false, 1.20f, 1 },
	{ "Medium",       true,  false, true,  true,  true,   0.5f, 2,   4,   false, true,  512,  512,  8,  16,   16,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 380.0f, true,  true,  false, false, 1.0f,   1.5f,   2, 48.0f,  true,   false, 0.12f, true, 0.0f, true, true,  false, false, 1,     false, false, false, true,  true , false, false, false, false, false, 1.0f, 0.6f, false, 1.20f, 2 },
	{ "High",         true,  false, true,  true,  true,   0.667f, 3,   6,   true,  true,  1024, 1200, 10,  64,   24,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 380.0f, true,  true,  true,  false, 1.0f,   1.5f,   2, 48.0f,  false,  true,  0.12f, true, 1.0f, true, true,  false, false, 1,     false, false, false, true,  true , false, false, false, false, false, 1.0f, 0.6f, false, 1.20f, 4 },
	{ "Ultra",        true,  true,  true,  true,  true,   0.75f, 4,   8,   true,  true,  2400, 2400, 11,  96,   32,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 480.0f, true,  true,  true,  true,  0.5f,   1.5f,   2, 48.0f,  false,  true,  0.12f, true, 1.0f, true, true,  true,  false, 1,     false, false, false, true,  true , false, false, false, false, false, 1.0f, 0.6f, false, 1.20f, 8 },
	{ "Nightmare", true, true, true, true,  true,   0.8f, 5,   10,  true,  true,  3600, 3600, 12, 128,  48,   0.05f, 0.0f,  0.7f, 1,  1.2f, 42.0f, true, 480.0f, true,  true,  true,  true,  0.5f, 1.5f,   2, 48.0f, false,  true,  0.12f, true, 1.0f, true, true,  true,  true,  0,     false, false, false, false, true , false, false, false, false, false, 1.0f, 0.6f, false, 1.20f, 16 },
};

// Ray-tracing augmentation layered on top of a raster tier when RTX is on and the device is
// RT-capable (Vulkan + ray-query). Indexed by raster tier. Everything here is VK + RT only;
// with RTX off, or on GL3 / a non-RT GPU, none of it is applied and the raster tier stands as
// the full picture. parallaxShadow / hdrGamma use -1 = "leave the raster tier's value". The
// Nightmare row carries the retired "Path Tracing" RT feature set.
struct RtAugment {
	bool  rtSunShadows;
	bool  rtMovingLights;
	bool  rtReflections;
	bool  rtao;
	float rtaoResScale;
	bool  rtgi;				// when true, replaces rtao (AO = GI with radiance 0)
	float rtgiResScale;		// r_rtgiResScale when rtgi on (inert otherwise): the GI gather/denoise resolution
	bool  rtShadowBlur;
	bool  rtAllLights;
	bool  rtBlurStagger;
	bool  volumetrics;
	int   volLights;		// r_volLights when volumetrics on (0 = leave the cvar): 2 = all lights (point lights too)
	bool  needVelocity;		// force r_motionVectors on so the NRD denoiser has motion vectors
	float parallaxShadow;	// RT-driven override of the raster value (-1 = keep raster)
	float hdrGamma;			// RT-driven override of the raster value (-1 = keep raster)
	float fsrScale;			// r_fsrScale when RTX on (the tier's default upscaling; 1 = Native-AA). A free
							// override - the Upscaling menu can still change it without reading "Custom"
	float blurMinPenumbra;	// r_rtShadowBlurMinPenumbra when rtShadowBlur on: skip the blur for lights whose
							// screen penumbra is finer than this (px). Free knob - the debug slider tweaks it
							// without reading "Custom" (not matched in Detect). Inert where rtShadowBlur is off
};

// RT shadows + AO scale from Medium up (RTAO auto-builds the RT scene via R_RtSceneWanted; RT shadows
// reuse it): Medium = RTAO @ 3/4 + HARD RT shadows on the sun + moving lights. High = + SOFT (blur)
// shadows, AO @ 4/5. Ultra drops RTAO for one-bounce RTGI (GI supersedes AO), keeps reflections, and
// extends RT shadows to ALL lights (staggered to stay affordable). Nightmare = all-light soft shadows
// at FULL rate (no stagger) + volumetrics. needVelocity is on wherever a denoised RT effect runs
// (RTAO or RTGI) so NRD gets motion vectors.
static const RtAugment rtAugment[PRESET_COUNT] = {
//                  sun    mov    refl   rtao   aoRes  rtgi   giRes  blur   all    stag   vol    volL  vel    parlxSh hdrGam fsrScale penumbra
/* Potato    */   { false, false, false, false, 1.0f,  false, 0.75f, false, false, false, false, 0,    false, -1.0f, -1.0f, 1.0f,  1.0f },
/* Low       */   { false, false, false, false, 1.0f,  false, 0.75f, false, false, false, false, 0,    false, -1.0f, -1.0f, 1.0f,  1.0f },
/* Medium    */   { true,  true,  false, true,  0.75f, false, 0.75f, false, false, false, false, 0,    true,  -1.0f, -1.0f, 1.0f,  1.0f },
/* High      */   { true,  true,  false, true,  0.8f,  false, 0.75f, true,  false, false, false, 0,    true,  -1.0f, -1.0f, 1.0f,  1.0f },
/* Ultra     */   { true,  true,  true,  false, 1.0f,  true,  0.667f,true,  true,  true,  false, 0,    true,  0.5f,  -1.0f, 1.0f,  4.0f },
/* Nightmare */   { true,  true,  true,  false, 0.75f, true,  0.75f, true,  true,  false, true,  2,    true,  0.5f,  1.0f,  0.75f, 2.0f },
};


static void ApplyEnhancementPreset( int idx, int rtMode )
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
	// Tonemap curve tracks the HDR flag: the HDR tiers (Medium+) get the DUDE signature curve,
	// the faithful floor (Potato/Low, HDR off) stays on straight-resolve mode 0. Written
	// explicitly so a hand-picked curve is reset to the tier's intent on preset switch.
	cvarSystem->SetCVarInteger( "r_hdrTonemap", p.hdr ? 5 : 0 );
	// HDR display gamma: preset-owned so Path Tracing can neutralize it (1.0) for its brighter,
	// GI-lit scene while every other tier keeps 1.20. Live shader uniform, shows immediately; inert
	// on Potato/Low (no curve). Exposure is a plain cvar default (1.3), not tiered.
	cvarSystem->SetCVarFloat( "r_hdrGamma", p.hdrGamma );
	r_pbr.SetBool( p.pbr );
	// Screen-space SSR is turned OFF at any tier that enables RT reflections (only Path Tracing): RT owns
	// every reflective pixel and the SSR march is skipped under it (2026-09-23), so leaving r_ssr on would be
	// dead weight and a confusing "SSR on" readout. The RT reflection toggle lives in its own settings group,
	// so this no longer hides it.
	r_ssr.SetBool( p.ssr && !p.rtReflections );
	r_ssrResScale.SetFloat( p.ssrResScale );
	// SSR vs RT reflection luminance: the ray-traced reflection (Path Tracing, rtReflections) reads
	// brighter than the screen-space one at the same intensity, so the SSR tiers use 0.8 and the RT tier
	// 0.5 to match. Lower/SSR-off tiers carry 0.5 (the cvar default), inert until SSR is hand-enabled.
	r_ssrIntensity.SetFloat( ( p.ssr && !p.rtReflections ) ? 0.8f : 0.5f );

	// GPU tessellation (Vulkan-only; inert on GL3): character smoothing from High,
	// normal-map displacement from Ultra.
	r_tessellation.SetBool( p.tessellation );
	r_tessDisplace.SetFloat( p.tessDisplace );

	// parallax occlusion mapping (GL3 + Vulkan): world relief from Medium, self-shadow from High.
	r_parallax.SetBool( p.parallax );
	r_parallaxShadow.SetFloat( p.parallaxShadow );

	// GPU MD5 skinning (Vulkan-only; inert on GL3). ON from Medium up (the DRAW offload; CPU position
	// skin still runs); Potato/Low keep it OFF because option-B TBN is a fidelity divergence and they
	// are the faithful floor. The cvar default is also on (Vulkan), so no-preset boots get it too.
	r_gpuSkinning.SetBool( p.gpuSkin );

	// per-object motion vectors (Vulkan-only; inert on GL3): Ultra/Nightmare, where the
	// SSR temporal accumulation consumes them and the normal prepass is already funded.
	r_motionVectors.SetBool( p.motionVectors );

	// FSR2 Native-AA (R1/E; Vulkan-only, inert on GL3): Nightmare only. The resolve
	// bypasses the row's SMAA while FSR2 runs, so there is no stacked AA.
	r_fsr.SetBool( p.fsr );

	// split-cache movers' cube: half res everywhere, full res on Nightmare only.
	r_shadowMapSplitDynDrop.SetInteger( p.splitDynDrop );

	// ray-traced shadows (Path Tracing only; VK + RT hardware, inert on GL3/non-RT):
	// RT sun + RT moving point lights. The cvars auto-build the persistent world scene.
	r_rtSunShadows.SetBool( p.rtSunShadows );
	r_rtMovingLights.SetBool( p.rtMovingLights );

	// RT reflections (Path Tracing only; VK + RT hardware, inert on GL3/non-RT). rtSunShadows above
	// built the persistent world BLAS this traces; the monster BLAS (r_rtAnimBlas) is default-on and inert
	// without a TLAS, so it is left alone here (never preset-forced off — see the struct note).
	r_rtReflections.SetBool( p.rtReflections );
	// RT reflection trace resolution: Half wherever the preset turns RT reflections on (Path Tracing) - a
	// quarter of the rays, temporally upscaled to full res with no visible loss (docs/rtx-reflections.md RR6c).
	// Inert on the other tiers, which get the Full default back.
	r_rtReflResScale.SetFloat( p.rtReflections ? 0.5f : 1.0f );
	r_ssaoTemporalTrade.SetBool( p.ssaoTemporalTrade );
	com_interpolate.SetBool( p.interpolate );
	r_rtao.SetBool( p.rtao );
	r_rtaoResScale.SetFloat( p.rtaoResScale );
	r_rtgi.SetBool( p.rtgi );		// H6e: Path Tracing enables GI (and carries rtao=false — GI replaces it)
	r_rtShadowBlur.SetBool( p.rtShadowBlur );
	r_rtAllLights.SetBool( p.rtAllLights );
	r_rtReStirDI.SetBool( false );			// dude_rtx 2 only (set in the RT overlay below)
	r_rtReStirDIApply.SetBool( false );
	r_rtgiCache.SetBool( false );			// irradiance cache: off at the raster/RTX-off baseline; follows RTGI in the RT overlay below
	r_rtgiCacheTail.SetBool( false );		// cache path-tail mode: off except Path Tracing (dude_rtx 2), set in the RT overlay below
	r_rtLightGrid.SetBool( false );			// RT light grid: Path Tracing (dude_rtx 2) only, set in the RT overlay below
	r_rtShadowBlurStagger.SetBool( p.rtBlurStagger );
	r_volumetrics.SetBool( p.volumetrics );

	// skin / flesh subsurface scattering (GL3 + Vulkan, all tiers): a wrapped diffuse that warms
	// the terminator on classifier-tagged skin/flesh. ON from Low up; Potato keeps it 0 (the
	// faithful floor). Only the strength is preset-owned — the reach (r_skinScatter) stays a free
	// tuning knob (Graphics -> Lighting), since it is pure taste with no cost or fidelity tier.
	r_skinShading.SetFloat( p.skinShading );

	// Max texture anisotropy (image_anisotropy; every backend). Set by name — it is a renderer cvar,
	// not one of the r_* handles this file holds. ChangeTextureFilter reclamps it to the device max,
	// and on VK BeginFrame's live poll rebuilds the samplers, so the preset takes effect without a
	// vid_restart. Potato/Low 1 (the faithful, no-AF floor); Medium 2; High 4; Ultra 8; Nightmare 16.
	cvarSystem->SetCVarInteger( "image_anisotropy", p.anisotropy );

	// --- Ray-tracing axis (orthogonal to the raster tier above) ---
	// The raster block left every RT cvar at its OFF baseline (the rows carry rt* = false). If RTX
	// is requested AND the device is RT-capable (VK + ray-query), overlay the tier-scaled RT set.
	// Everything here is inert on GL3 / non-RT GPUs, so a non-capable machine always sees the pure
	// raster tier even if dude_rtx was somehow left on.
	const bool rtOn = ( rtMode != RTX_OFF ) && R_SupportsRayTracing();
	if ( rtOn ) {
		const RtAugment& a = rtAugment[idx];
		r_rtSunShadows.SetBool( a.rtSunShadows );
		r_rtMovingLights.SetBool( a.rtMovingLights );
		r_rtReflections.SetBool( a.rtReflections );
		r_rtao.SetBool( a.rtao );
		r_rtaoResScale.SetFloat( a.rtaoResScale );
		r_rtgi.SetBool( a.rtgi );
		// GI gather/denoise resolution is a per-tier cost lever (Ultra 2/3, Nightmare 3/4), so the
		// preset owns it where GI runs; where GI is off it stays a free knob (left untouched).
		if ( a.rtgi ) {
			r_rtgiResScale.SetFloat( a.rtgiResScale );
			// Eye adaptation (auto-exposure) defaults ON wherever RTGI runs (the top RT tiers): GI lifts the
			// blacks in lit rooms, so the scene-driven exposure reads better there. A free knob - not matched
			// in Detect, so the user can switch it off without the preset reading "Custom".
			cvarSystem->SetCVarBool( "r_hdrEyeAdaptation", true );
		}
		r_rtShadowBlur.SetBool( a.rtShadowBlur );
		// sub-pixel-penumbra skip threshold: a per-tier default (Ultra skips harder at 4 px for a
		// cheaper blur, Nightmare softens more lights at 2 px) owned only where the blur runs. It
		// stays a free knob (the debug slider changes it without reading "Custom", and it is not
		// matched in Detect), so leave it untouched on tiers that don't blur.
		if ( a.rtShadowBlur ) {
			r_rtShadowBlurMinPenumbra.SetFloat( a.blurMinPenumbra );
		}
		r_rtAllLights.SetBool( a.rtAllLights );
		r_rtShadowBlurStagger.SetBool( a.rtBlurStagger );
		r_volumetrics.SetBool( a.volumetrics );
		// which lights scatter in the haze: a free tuning knob, but the top tier defaults it to
		// "all lights" (point lights too). 0 = leave the user's value. Only meaningful with volumetrics on.
		if ( a.volLights > 0 ) {
			cvarSystem->SetCVarInteger( "r_volLights", a.volLights );
		}
		// dude_rtx 2 "Path Tracing" (H7, docs/rtx-restir-di.md): ReSTIR direct lighting replaces the per-light
		// forward draws (r_rtAllLights' ray-per-light loop stays for the fast-changing lights DI leaves forward),
		// and where the tier runs RTGI the bounce rides the same gather + denoise (r_rtReStirDICombineGI).
		const bool fullRt = ( rtMode == RTX_PATHTRACED );
		r_rtReStirDI.SetBool( fullRt );
		r_rtReStirDIApply.SetBool( fullRt );
		// Irradiance cache (docs/rtx-irradiance-cache.md): the multi-bounce GI fill + a smoother bounce signal.
		// Enabled wherever RTGI runs (Ultra / Nightmare and the Path Tracing tier) - those high-end RT tiers can
		// afford the cache's VRAM + fill cost, and it helps GI quality/stability. Off where GI is off (inert anyway).
		r_rtgiCache.SetBool( a.rtgi );
		// Path-tail mode: a converged cell SERVES the bounce hit (skips its light loop + shadow ray) for steadier
		// frame pacing. It needs ReSTIR DI lighting the scene, so it is Path-Tracing-only - on at PT, off on every
		// other tier (and inert without the cache / DI there anyway).
		r_rtgiCacheTail.SetBool( fullRt );
		// RT light grid (docs/rtx-light-grid.md): per-cell importance-sampled light candidates replace the DI gather's
		// exhaustive per-pixel RIS over every light - a measured perf win (gather -29% at 40 lights, more with more
		// lights) with no visible cost. Needs DI, so it rides the Path Tracing tier; on there, off below.
		r_rtLightGrid.SetBool( fullRt );

		// RT reflections own every reflective pixel, so SSR is turned off under them and the
		// RT-vs-SSR luminance match + half-res reflection trace follow (mirrors the raster
		// derivation above, but against the augment's rtReflections instead of the row's).
		r_ssr.SetBool( p.ssr && !a.rtReflections );
		r_ssrIntensity.SetFloat( ( p.ssr && !a.rtReflections ) ? 0.8f : 0.5f );
		r_rtReflResScale.SetFloat( a.rtReflections ? 0.5f : 1.0f );

		// The NRD denoiser (RTAO / RTGI) needs motion vectors; force the velocity producer on
		// where the raster tier didn't already fund it (e.g. Ultra, which carries mv/fsr off).
		if ( a.needVelocity ) {
			r_motionVectors.SetBool( true );
		}

		// RT-driven overrides of raster look cvars (-1 = keep the tier's value): ease POM
		// self-shadow where RTAO/GI already occludes crevices, and neutralize HDR display gamma
		// where the GI bounce lifts overall radiance.
		if ( a.parallaxShadow >= 0.0f ) {
			r_parallaxShadow.SetFloat( a.parallaxShadow );
		}
		if ( a.hdrGamma >= 0.0f ) {
			cvarSystem->SetCVarFloat( "r_hdrGamma", a.hdrGamma );
		}
		// The tier's default FSR2 upscaling (Nightmare+RTX = 75%). Inert where the tier's r_fsr is off
		// (Medium/High/Ultra+RTX). Not in Detect, so the Upscaling menu can still override it freely
		// without the preset reading "Custom".
		r_fsrScale.SetFloat( a.fsrScale );
	}
}

// Return the raster tier whose cvar vector the live cvars currently match, or -1 for "Custom"
// (any hand-tweak since a preset was last applied), and, through outRtMode, the detected RT axis
// state (RTX_OFF / RTX_ON). The two axes are detected independently: the raster columns are
// compared against the tier row, and the RT-overlay columns against either their OFF baseline or
// the tier's rtAugment row, depending on whether any RT feature is live. Every (tier, mode)
// combination is a unique vector, so a match is unambiguous.
static int DetectEnhancementPreset( int* outRtMode = NULL )
{
	// Classify the live RT axis first. Any augment-driven RT cvar being on means RTX is on
	// (PATHTRACED / H7 is not built yet, so today the axis is only OFF or ON). On a non-RT device
	// none of these can be on, so this reads OFF there.
	const bool diLive = r_rtReStirDI.GetBool() && r_rtReStirDIApply.GetBool();	// dude_rtx 2: DI lights the scene
	const bool rtLive =
		r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() || r_rtReflections.GetBool() ||
		r_rtao.GetBool() || r_rtgi.GetBool() || r_rtShadowBlur.GetBool() ||
		r_rtAllLights.GetBool() || r_volumetrics.GetBool() || r_rtReStirDI.GetBool();
	const int rtMode = diLive ? RTX_PATHTRACED : ( rtLive ? RTX_ON : RTX_OFF );
	if ( outRtMode ) {
		*outRtMode = rtMode;
	}

	for ( int i = 0; i < PRESET_COUNT; i++ ) {
		const EnhancementPreset& p = enhancementPresets[i];

		// Pure raster columns - everything the RT overlay never touches.
		const bool rasterMatch =
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
			cvarSystem->GetCVarInteger( "r_hdrTonemap" ) == ( p.hdr ? 5 : 0 ) &&
			r_pbr.GetBool()                    == p.pbr &&
			idMath::Fabs( r_ssrResScale.GetFloat() - p.ssrResScale ) < 0.01f &&
			r_tessellation.GetBool()           == p.tessellation &&
			idMath::Fabs( r_tessDisplace.GetFloat() - p.tessDisplace ) < 0.01f &&
			r_parallax.GetBool()               == p.parallax &&
			r_gpuSkinning.GetBool()            == p.gpuSkin &&
			r_fsr.GetBool()                    == p.fsr &&
			r_shadowMapSplitDynDrop.GetInteger() == p.splitDynDrop &&
			r_ssaoTemporalTrade.GetBool()      == p.ssaoTemporalTrade &&
			com_interpolate.GetBool()          == p.interpolate &&
			cvarSystem->GetCVarInteger( "image_anisotropy" ) == p.anisotropy &&
			idMath::Fabs( r_skinShading.GetFloat() - p.skinShading ) < 0.01f;
		if ( !rasterMatch ) {
			continue;
		}

		// RT-overlay columns - expected values depend on the RT axis state.
		bool overlayMatch;
		if ( rtMode == RTX_OFF ) {
			// Pure raster tier: every RT cvar off, and the overlay-driven look cvars at their
			// raster-row values.
			overlayMatch =
				!r_rtSunShadows.GetBool() && !r_rtMovingLights.GetBool() &&
				!r_rtReflections.GetBool() && !r_rtao.GetBool() && !r_rtgi.GetBool() &&
				!r_rtShadowBlur.GetBool() && !r_rtAllLights.GetBool() &&
				!r_rtShadowBlurStagger.GetBool() && !r_volumetrics.GetBool() &&
				!r_rtReStirDI.GetBool() && !r_rtReStirDIApply.GetBool() && !r_rtgiCache.GetBool() && !r_rtLightGrid.GetBool() &&
				r_ssr.GetBool()                    == p.ssr &&
				idMath::Fabs( r_ssrIntensity.GetFloat() - ( p.ssr ? 0.8f : 0.5f ) ) < 0.01f &&
				idMath::Fabs( r_rtReflResScale.GetFloat() - 1.0f ) < 0.01f &&
				r_motionVectors.GetBool()          == p.motionVectors &&
				idMath::Fabs( cvarSystem->GetCVarFloat( "r_hdrGamma" ) - p.hdrGamma ) < 0.01f &&
				idMath::Fabs( r_parallaxShadow.GetFloat() - p.parallaxShadow ) < 0.01f;
		} else {
			// RTX on: the RT cvars must match this tier's augment, and the overridden look cvars
			// the augment value where it sets one (>= 0), else the raster-row value.
			const RtAugment& a = rtAugment[i];
			const bool  expSsr   = p.ssr && !a.rtReflections;
			const bool  expMv    = a.needVelocity ? true : p.motionVectors;
			const float expParlx = ( a.parallaxShadow >= 0.0f ) ? a.parallaxShadow : p.parallaxShadow;
			const float expGamma = ( a.hdrGamma      >= 0.0f ) ? a.hdrGamma      : p.hdrGamma;
			overlayMatch =
				r_rtSunShadows.GetBool()           == a.rtSunShadows &&
				r_rtMovingLights.GetBool()         == a.rtMovingLights &&
				r_rtReflections.GetBool()          == a.rtReflections &&
				idMath::Fabs( r_rtReflResScale.GetFloat() - ( a.rtReflections ? 0.5f : 1.0f ) ) < 0.01f &&
				r_rtao.GetBool()                   == a.rtao &&
				( !a.rtao || idMath::Fabs( r_rtaoResScale.GetFloat() - a.rtaoResScale ) < 0.01f ) &&
				r_rtgi.GetBool()                   == a.rtgi &&
				( !a.rtgi || idMath::Fabs( r_rtgiResScale.GetFloat() - a.rtgiResScale ) < 0.01f ) &&
				r_rtShadowBlur.GetBool()           == a.rtShadowBlur &&
				r_rtAllLights.GetBool()            == a.rtAllLights &&
				r_rtShadowBlurStagger.GetBool()    == a.rtBlurStagger &&
				r_volumetrics.GetBool()            == a.volumetrics &&
				r_rtReStirDI.GetBool()             == ( rtMode == RTX_PATHTRACED ) &&
				r_rtReStirDIApply.GetBool()        == ( rtMode == RTX_PATHTRACED ) &&
				r_rtgiCache.GetBool()              == a.rtgi &&
				r_rtLightGrid.GetBool()            == ( rtMode == RTX_PATHTRACED ) &&
				r_ssr.GetBool()                    == expSsr &&
				idMath::Fabs( r_ssrIntensity.GetFloat() - ( expSsr ? 0.8f : 0.5f ) ) < 0.01f &&
				r_motionVectors.GetBool()          == expMv &&
				idMath::Fabs( cvarSystem->GetCVarFloat( "r_hdrGamma" ) - expGamma ) < 0.01f &&
				idMath::Fabs( r_parallaxShadow.GetFloat() - expParlx ) < 0.01f;
		}

		if ( overlayMatch ) {
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

		// Volumetric lighting (docs/volumetric-lighting.md). Same idiom as the Shadows / Ambient Occlusion /
		// Reflections groups' RT sections: greyed on the legacy/GL3 backend and non-RT GPUs, where the cvars
		// are inert. Preset-owned on/off (on at Path Tracing only), so flipping it by hand reads "Custom".
		{
			const bool rtCapable = R_SupportsRayTracing();
			ImGui::SeparatorText( rtCapable ? "Ray Tracing (RTX)" : "Ray Tracing (RTX) - needs Vulkan + RT GPU" );
			ImGui::BeginDisabled( !rtCapable );

			bool vol = CvarCheckbox( "Volumetric Lighting", r_volumetrics, "The air scatters the scene's real lights toward you: visible cones under spot lights, "
				"shafts through grates, monsters and railings cutting dark wedges out of the lit haze, a "
				"flashlight beam you can see. Changes the look of the game - the stock game has no lit air, and "
				"haze lifts the blacks inside lit volumes; only the Path Tracing preset turns it on. Costs about "
				"0.5 ms with spot lights only, ~1.2 ms with all lights (1440p). Needs the Vulkan backend, "
				"ray-query (RT) hardware and HDR; shadows in the haze need the RT scene (RT Sun Shadows on)." );
			if ( rtCapable && vol && !r_rtSunShadows.GetBool() && !r_rtAllLights.GetBool() && !r_rtao.GetBool() ) {
				ImGui::TextDisabled( "  (no RT scene: the haze is unshadowed - enable RT Sun Shadows)" );
			}

			ImGui::BeginDisabled( !vol );
			// discrete-stop slider (the Resolution sliders' idiom): r_volLights 1 / 2
			const char *volLightLabels[] = { "Spot lights only", "All lights" };
			int volLights = r_volLights.GetInteger() >= 2 ? 1 : 0;
			if ( ImGui::SliderInt( "Lights In The Haze", &volLights, 0, 1, volLightLabels[volLights], ImGuiSliderFlags_NoInput ) ) {
				volLights = volLights < 0 ? 0 : ( volLights > 1 ? 1 : volLights );
				r_volLights.SetInteger( volLights + 1 );
			}
			AddTooltip( "Spot lights only: just the projected lights (and sun-style parallel lights) - the "
				"authored feature lights whose cones read as shafts; cheapest, but the stock maps have few of "
				"them. All lights: point lights too, as a fainter ambience (Point Light Haze below) - far more "
				"presence, about twice the cost." );

			CvarSliderF( "Haze Density", r_volDensity, 0.0f, 4.0f, "%.2f", "How much haze is in the air. Low = a hint of light in the cones; high = a smoke machine." );

			ImGui::BeginDisabled( r_volLights.GetInteger() < 2 );
			CvarSliderF( "Point Light Haze", r_volPointLightScale, 0.0f, 1.0f, "%.2f", "Brightness of point lights in the haze relative to spot lights. Rooms hold many large "
				"fill lights; kept low they are a faint ambience under the spot-light cones instead of drowning them." );
			CvarSliderF( "Point Light Haze In Fog", r_volFogPointLightScale, 0.0f, 1.0f, "%.2f", "What Point Light Haze rises to inside the game's own fog volumes: foggy rooms hold more "
				"haze, so their lights glow more in the air. Follows each fog's thickness and fades in at its edges." );
			ImGui::EndDisabled();

			CvarSliderF( "Flashlight Beam", r_volFlashlightScale, 0.0f, 2.0f, "%.2f", "Brightness of your own flashlight beam (and muzzle flashes) in the haze. The beam starts "
				"at your eyes, so it fogs the view more than any other light; 0 = the flashlight lights surfaces "
				"but never the air." );
			CvarSliderF( "AO Intensity With Haze", r_volAoIntensity, 0.0f, 3.0f, "%.2f", "Ambient-occlusion intensity used while Volumetric Lighting is on (when higher than the "
				"Ambient Occlusion group's own intensity). The haze adds light inside lit volumes and flattens "
				"contact shading; stronger AO puts the grounding back. Your AO settings are not changed - turn "
				"the haze off and they apply again. 0 = leave AO alone." );
			ImGui::EndDisabled();	// volumetrics off

			ImGui::EndDisabled();	// !rtCapable
		}
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
			"taken from each material's authored height map, or built from its normal map\n"
			"when the normal carries the detail, then marched to fake real depth. Opaque\n"
			"world (BSP) geometry only - props and characters are left to Tessellation,\n"
			"where parallax would deform their curved surfaces. Takes effect on the next\n"
			"map load. Off = vanilla." ) ) {
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

// Antialiasing group: hardware MSAA + one effective post-AA selector under one
// "Antialiasing" header. Drawn OUTSIDE the enhancement-gated block so hardware MSAA stays
// usable on the legacy backend; the post part is opengl3/Vulkan-only and greys out on the
// legacy renderer. The selector mirrors the classic System-menu AA row: ONE choice —
// Off / FXAA / SMAA / FSR2 (VK) — even though two cvars sit underneath (r_fsr wins at the
// resolve; r_rhiAA is kept as the GL3 / dispatch-failure fallback).
// Apply one effective AA selection to the archived truth. Shared by the classic menu's
// dudeAA command and the ImGui Antialiasing dropdown: 0-2 = r_rhiAA (FSR off), 3 = FSR2
// (r_rhiAA left untouched — it is bypassed while FSR2 runs and is the GL3 / dispatch-
// failure fallback, so switching back restores the previous post AA).
static void Com_ApplyDudeAA( int v )
{
	if ( v >= 3 ) {
		r_fsr.SetBool( true );
		return;
	}
	r_fsr.SetBool( false );
	r_rhiAA.SetInteger( idMath::ClampInt( 0, 2, v ) );
}

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

		// Post AA — ONE effective selection (mirrors the classic System-menu AA row).
		// opengl3/Vulkan only, so it greys out on the legacy backend; the FSR2 entry only
		// exists on Vulkan. Underneath, FSR2 is r_fsr (r_rhiAA kept as the GL3/fallback
		// post AA — bypassed while FSR2 runs, restored when switching back).
		const bool isVulkan = glConfig.rhiBackend && !glConfig.coreProfile;
		ImGui::BeginDisabled( !R_BackendSupportsEnhancements() );
		int aa = ( isVulkan && r_fsr.GetBool() ) ? 3 : idMath::ClampInt( 0, 2, r_rhiAA.GetInteger() );
		ImGui::SetNextItemWidth( 220.0f );
		const char *aaItems = isVulkan
			? "Off\0FXAA (fast, damps shimmer)\0SMAA (sharpest edges)\0FSR2 TAA (kills shimmer, temporal)\0"
			: "Off\0FXAA (fast, damps shimmer)\0SMAA (sharpest edges)\0";
		if ( ImGui::Combo( "Post Antialiasing", &aa, aaItems ) ) {
			Com_ApplyDudeAA( aa );
		}
		AddTooltip( "Post antialiasing over the finished 3D view, on top of (and independent from) "
			"the hardware MSAA above; one effective choice. FXAA is one cheap pass whose subpixel "
			"smoothing also damps the specular/normal-map shimmer MSAA can't touch, at a slight "
			"overall softening. SMAA reconstructs edges much more precisely and leaves texture detail "
			"sharp, but does not treat shimmer. FSR2 TAA (Vulkan only, non-vanilla) is AMD FSR2 at "
			"native resolution: temporal accumulation + RCAS that resolves the shimmer structurally — "
			"auto-enables motion vectors + sub-pixel jitter, ~1.7 ms GPU at 1440p, and keeps your "
			"SMAA/FXAA choice as the fallback where it can't run. HUD and menus are never affected. "
			"(docs/fsr-temporal-pipeline.md)" );

		// Strength drives FXAA's subpixel term (the part that actually chases shimmer): 0 =
		// edge-only FXAA (sharpest), higher = more subpixel smoothing at some texture softening.
		ImGui::BeginDisabled( aa != 1 );
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

		// FSR2 sub-options — only meaningful while FSR2 TAA is the selection above.
		{
			ImGui::BeginDisabled( aa != 3 );
			float sharp = r_fsrSharpness.GetFloat();
			ImGui::SetNextItemWidth( 220.0f );
			if ( ImGui::SliderFloat( "RCAS Sharpness", &sharp, 0.0f, 1.0f, "%.2f" ) ) {
				r_fsrSharpness.SetFloat( sharp );
			}
			AddTooltip( "FSR2's built-in sharpening pass, countering the slight softening of any "
				"temporal accumulation. 0 disables the pass. (r_fsrSharpness)" );
			CvarCheckbox( "Reactive Mask", r_fsrReactive, "Deghosts additive/translucent content (particles, muzzle flashes, GUI "
				"screens): an opaque-only snapshot is compared against the final frame, and where "
				"they diverge FSR2 trusts its history less. Leave on. (r_fsrReactive)" );
			{
				ImGui::BeginDisabled( !r_fsrReactive.GetBool() );
				float rs = r_fsrReactiveScale.GetFloat();
				ImGui::SetNextItemWidth( 220.0f );
				if ( ImGui::SliderFloat( "Reactive Strength", &rs, 0.0f, 2.0f, "%.2f" ) ) {
					r_fsrReactiveScale.SetFloat( rs );
				}
				AddTooltip( "Higher = less ghosting on particles but more shimmer on them. "
					"(r_fsrReactiveScale)" );
				ImGui::EndDisabled();
			}
			ImGui::EndDisabled();	// aa != 3 (FSR2 not selected)
		}
		ImGui::EndDisabled();	// post-process supported gate
		EndSettingsGroup();
	}
}

static void DrawEnhGroup_AmbientOcclusion()
{
	// Ambient Occlusion (DUDE Phase 3.5). Master toggle + the quality/perf stops; the fine
	// tuning sliders live in the Debugging tab. Read live by the renderer (no restart).
	if ( BeginSettingsGroup( "Ambient Occlusion" ) ) {
		CvarCheckbox( "SSAO (GTAO ambient occlusion)", r_ssao, "Screen-space ambient occlusion (GTAO): darkens creases, corners and contact "
			"points in the ambient light only, so models look grounded instead of flat/plastic. It "
			"never touches direct/dynamic lights, so it stays correct as lighting changes. Tuning "
			"sliders are in the Developer tab. Non-vanilla; opengl3 only." );

		// Resolution: discrete quality/perf stops from half to full screen resolution.
		ImGui::BeginDisabled( !r_ssao.GetBool() );
		// AO is one technique at a time: RT Ambient Occlusion or RT Global Illumination (below) each REPLACE
		// the screen-space GTAO, so its sampling sliders are inert while either is on.
		ImGui::BeginDisabled( r_rtao.GetBool() || r_rtgi.GetBool() );
		const float ssaoResStops[]  = { 0.5f, 0.667f, 0.75f, 0.8f, 1.0f };
		const char *ssaoResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Four-fifths (4/5)", "Full" };
		CvarStopsSliderF( "Resolution", r_ssaoResScale, ssaoResStops, ssaoResLabels, IM_ARRAYSIZE( ssaoResStops ) );
		AddTooltip( "Resolution the AO buffer is computed at, as a fraction of the screen. Half is "
			"~4x cheaper and a little softer; Full is sharpest and most expensive; 3/4 and 4/5 sit "
			"between. Lower this first if SSAO costs too much." );

		// Temporal accumulation: reuse the previous frame's AO (reprojected by camera
		// motion) so the horizon-search noise settles and slices/steps can run lower.
		CvarCheckbox( "Temporal Accumulation", r_ssaoTemporal, "Blend AO across frames (reprojected as the camera moves) instead of recomputing "
			"it fresh each frame. Smooths the AO and lets the Directions/Steps run lower for the same "
			"look, so it's cheaper on weaker GPUs. Ghosting on fast motion is clamped automatically; "
			"the Feedback strength lives in the Developer tab. Non-vanilla; opengl3 only." );

		// Depth-mip acceleration (docs/ssao-perf-optimization.md). A prefiltered linear-depth
		// mip chain: far horizon steps read a coarse, cache-local mip instead of scattering
		// across full-res depth. ~40% cheaper AO at a wide radius (measured r_ssaoDepthMip
		// 0 vs 1 = 74 -> 104 fps). Trade-off: a faint silhouette halo. Standalone preference,
		// not preset-driven — a preset change shouldn't override the fidelity choice.
		CvarCheckbox( "Depth-Mip Acceleration", r_ssaoDepthMip, "Speed up the AO horizon search by reading a prefiltered depth mip chain: far "
			"samples read a coarser, cache-friendly copy of the depth buffer instead of scattering "
			"across full-resolution depth. Big win at a wide radius (~40% cheaper AO here). "
			"Trade-off: a faint dark halo can appear around object silhouettes, since one coarse "
			"depth texel can't represent both surfaces at an edge. Off = the exact full-resolution "
			"depth march (most accurate, most expensive). Recommended on. Non-vanilla; opengl3/Vulkan." );
		ImGui::EndDisabled();	// SSAO sliders superseded by RTAO / RTGI

		// Ray-traced AO (docs/rtx-rtao.md H4). Same idiom as the Shadows group's RT section:
		// greyed on the legacy/GL3 backend and non-RT GPUs, where the cvars are inert.
		// Enabling it REPLACES the screen-space producer above (those sampling sliders stop
		// applying); intensity and the light-response sliders in the Developer tab still do.
		{
			const bool rtCapable = R_SupportsRayTracing();
			ImGui::SeparatorText( rtCapable ? "Ray Tracing (RTX)" : "Ray Tracing (RTX) - needs Vulkan + RT GPU" );
			ImGui::BeginDisabled( !rtCapable );

			// RT GI supersedes RT AO (below): one denoiser, and GI grounds the scene itself. Grey RTAO out
			// while GI is on so it is clear only one runs.
			ImGui::BeginDisabled( r_rtgi.GetBool() );
			CvarCheckbox( "RT Ambient Occlusion", r_rtao, "Replace the screen-space occlusion above with one traced hemisphere ray per "
				"pixel against the real scene, denoised across frames: contact shading from geometry "
				"that is off-screen or behind the camera, no screen-edge artifacts, and cheaper than "
				"the full-resolution march at the top tiers. Has its OWN Intensity below (neutral "
				"1.0 — the traced result needs no boost); the sampling controls above "
				"(Directions/Steps/Resolution, Depth-Mip) are screen-space-only and stop mattering. "
				"Needs Motion Vectors or FSR on for the denoiser; enabling it builds the ray-tracing "
				"scene for the map. Vulkan + RT hardware only." );

			// Ray Length (r_rtaoRadius) lives in the Developer tab's RTAO section, not here.
			CvarSliderF( "Intensity##rtao", r_rtaoIntensity, 0.0f, 4.0f, "%.2f", "RTAO strength. The traced occlusion is already physically grounded, so the "
				"neutral 1.00 default shows it as-is — unlike the screen-space Intensity (1.2), "
				"which compensates for the march and applies only when RTAO is off. Raise for "
				"deeper creases, lower to soften." );

			// Resolution: RTAO runs the rays AND the NRD denoiser (its dominant cost) at this fraction
			// of the screen, then upscales the occlusion. AO is low-frequency, so fractional stops work
			// without the strobing that forces reflections to integer divisors.
			const float rtaoResStops[]  = { 0.5f, 0.667f, 0.75f, 0.8f, 1.0f };
			const char *rtaoResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Four-fifths (4/5)", "Full" };
			CvarStopsSliderF( "Resolution##rtao", r_rtaoResScale, rtaoResStops, rtaoResLabels, IM_ARRAYSIZE( rtaoResStops ) );
			AddTooltip( "Resolution RTAO runs its rays AND the NRD denoiser at, as a fraction of the "
				"screen; the occlusion is upscaled to full res. The denoiser is the bulk of RTAO's "
				"cost, so Three-quarter is ~a third cheaper and Half ~two-thirds cheaper for little "
				"visible change (AO is low-frequency). Lower this first if RTAO costs too much. "
				"Default Full; Path Tracing uses Three-quarter." );

			ImGui::EndDisabled();	// RTAO greyed while RTGI on

			// Ray-traced GI (docs/rtx-rtgi.md H6). One traced bounce of indirect diffuse, denoised through
			// the SAME NRD instance as RTAO - so enabling it makes GI REPLACE RTAO (and the screen-space GTAO):
			// GI grounds the scene with real one-bounce occlusion + bounce light, so no AO pass runs alongside.
			ImGui::Spacing();
			CvarCheckbox( "RT Global Illumination", r_rtgi, "One traced bounce of indirect light, denoised: surfaces are lit by the colour "
				"bouncing off everything around them (a red panel tints the nearby grey wall), and dark "
				"corners fill in softly the way real light does - which Doom 3 never computed. REPLACES both "
				"RT Ambient Occlusion AND screen-space SSAO while on - it grounds the scene with real "
				"one-bounce occlusion + bounce light, so no separate AO pass runs. Needs Motion Vectors or "
				"FSR on for the denoiser, and builds the ray-tracing scene. NON-VANILLA look - lifts blacks "
				"in lit rooms. Vulkan + RT only." );

			CvarSliderF( "Intensity##rtgi", r_rtgiIntensity, 0.0f, 4.0f, "%.2f", "Strength of the bounced light added to the scene. 1.00 is the physical one-bounce "
				"result; raise for punchier colour bleed, lower for a subtle fill." );

			CvarSliderF( "Fog Awareness##rtgi", r_rtgiFogAttenuate, 0.0f, 4.0f, "%.2f", "Fades the bounced light out with distance through fog, so GI stops washing out "
				"Doom 3's distance fog (the far haze reads thin without it). 1.00 is the physical amount; "
				"raise toward 4 for a stronger foggy look (needed where a map's fog distance is huge and "
				"1.00 is too subtle); 0.00 lets GI ignore fog (the old look). Only matters in foggy areas." );

			// Ray Length (r_rtgiRadius) and Resolution (r_rtgiResScale) live in the Developer tab's RT GI
			// section, not here - this user-facing tab keeps just the look knobs + GI Shadows.
			CvarCheckbox( "GI Shadows", r_rtgiShadow, "Trace one shadow ray at each bounce point toward the brightest light reaching it, so a "
				"light behind a thin wall stops leaking bounced light through it. One extra ray per pixel; off "
				"is a little cheaper but lets light bleed through walls." );

			ImGui::EndDisabled();
			// explain the gray-out cases the checkbox itself can't (mirrors the RT Reflections hint)
			if ( rtCapable && r_rtao.GetBool() && !r_rtgi.GetBool()
					&& !( r_motionVectors.GetBool() || r_fsr.GetBool() ) ) {
				ImGui::TextDisabled( "  (needs Motion Vectors or FSR on for the denoiser - using screen-space AO)" );
			}
			if ( rtCapable && r_rtgi.GetBool()
					&& !( r_motionVectors.GetBool() || r_fsr.GetBool() ) ) {
				ImGui::TextDisabled( "  (GI needs Motion Vectors or FSR on for the denoiser - showing raw grainy GI)" );
			}
		}
		ImGui::EndDisabled();

		// Baked occlusion maps: independent of SSAO (works with it off). Inert unless a
		// material ships an occlusionmap stage, which no stock Doom 3 asset does. Strength
		// sliders live in the Developer tab, like the SSAO tuning.
		CvarCheckbox( "Baked Occlusion Maps", r_occlusionMaps, "Use per-material baked ambient-occlusion textures (the `occlusionmap` material "
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
	// Reflections (DUDE PBR Phase C.2, docs/ssr.md + docs/rtx-reflections.md). ONE group, no header master:
	// screen-space SSR and ray-traced reflections are independent techniques with their own switch inside (RT
	// owns every reflective pixel and the SSR march is skipped when it renders). Quality/perf stops here; the
	// fine tuning lives in the Developer tab next to the PBR knobs.
	if ( BeginSettingsGroup( "Reflections" ) ) {
		// Screen-space SSR on/off (was the group-header master); its sub-controls below gray out with it.
		CvarCheckbox( "Screen Space Reflections", r_ssr, "Glossy and metallic surfaces (polished floors, bare metal - per the PBR material table) "
			"mirror the on-screen scene: fixtures, screens, characters. Being screen-space, off-screen objects "
			"can't appear and rays fade at the screen edges. Superseded by Ray-Traced Reflections below when "
			"that is on - it owns every reflective pixel and this march is skipped. Works with PBR shading on "
			"or off; tuning sliders are in the Developer tab. Non-vanilla; opengl3 only." );
		ImGui::BeginDisabled( !r_ssr.GetBool() );
		// Resolution: discrete INTEGER-DIVISOR stops for the reflection buffer. Only 1/4, 1/2, Full —
		// fractional ratios (2/3, 3/4) make the reflection upscale beat into strobing/stripes (the
		// renderer snaps to these divisors regardless, docs/rtx-reflections.md RR7).
		const float ssrResStops[]  = { 0.25f, 0.5f, 1.0f };
		const char *ssrResLabels[] = { "Quarter (1/4)", "Half (1/2)", "Full" };
		CvarStopsSliderF( "Resolution##ssr", r_ssrResScale, ssrResStops, ssrResLabels, IM_ARRAYSIZE( ssrResStops ) );
		AddTooltip( "Resolution the reflection rays are marched at, as a fraction of the screen. "
			"The material response (Fresnel, gloss) always applies at full resolution, so lower "
			"stops only soften the reflected image — Half is ~4x cheaper and pairs well with "
			"Temporal Accumulation. Lower this first if reflections cost too much." );

		// Temporal accumulation: rotate the march jitter per frame and average the
		// results (reprojected by camera motion) so the grain resolves.
		CvarCheckbox( "Temporal Accumulation##ssr", r_ssrTemporal, "Blend reflections across frames (reprojected as the camera moves) instead of "
			"recomputing them fresh each frame. The march's grainy sparkle settles into a clean "
			"image; ghosting on fast motion is clamped automatically. The Feedback strength lives "
			"in the Developer tab. Non-vanilla; opengl3 only." );

		// Hi-Z acceleration (docs/ssao-perf-optimization.md, r_ssrHiZ). A min-Z depth pyramid
		// lets the march leap provably-empty span instead of stepping it. Pure perf option —
		// reflections are pixel-identical. Marginal + scene-dependent, so default-off + opt-in;
		// leap aggressiveness is the dev cvar r_ssrHiZLevel.
		CvarCheckbox( "Hi-Z Acceleration##ssr", r_ssrHiZ, "Speed up the reflection ray-march by leaping over empty space with a depth "
			"pyramid instead of stepping through it. Reflections look identical — this is a pure "
			"performance option, no visual change. The gain is scene-dependent: a few percent on "
			"open / distant reflections, and essentially nothing on grazing reflective floors "
			"(where the rays hug the surface). Best left off on older GPUs. Non-vanilla; opengl3/Vulkan." );

		// Glass: baked room probes replace the generic env/gen* cubemap (docs/ssr.md).
		CvarCheckbox( "Glass Reflections##ssr", r_ssrGlassProbes, "Glass reflects a snapshot of the actual room (captured automatically the "
			"first time you enter an area, cached to disk) instead of Doom 3's generic blurry "
			"cubemap - panes mirror the real room at any viewing angle. Re-capture an area with "
			"the bakeGlassProbe console command; brightness slider in the Developer tab. "
			"Non-vanilla; opengl3 only." );
		ImGui::EndDisabled();	// end SSR sub-controls (grayed with Screen Space Reflections)

		// Ray Tracing (RTX): ray-traced reflections in the SAME group, their own switch. RT is decoupled from
		// r_ssr (owns every reflective pixel; the SSR march is skipped when it renders), so its switch + knobs
		// stay live even with Screen Space Reflections off - Path Tracing runs RT with SSR off.
		// Gray out unless the prerequisites are met: VK + ray-query hardware and a built RT scene / TLAS
		// (r_rtSunShadows builds it; no longer needs r_ssr). Only Path Tracing turns the RT scene on, so
		// lower tiers read as unavailable.
		const bool rtCapable  = R_SupportsRayTracing();
		ImGui::SeparatorText( rtCapable ? "Ray Tracing (RTX)" : "Ray Tracing (RTX) - needs Vulkan + RT GPU" );
		const bool rtIsVulkan = rtCapable && glConfig.rhiBackend && !glConfig.coreProfile;
		const bool rtSceneOn  = r_rtSunShadows.GetBool();
		ImGui::BeginDisabled( !rtIsVulkan || !rtSceneOn );
		CvarCheckbox( "Ray-Traced Reflections", r_rtReflections, "Trace the scene to reflect the real room + monsters, including geometry off-screen "
			"or hidden from the camera that screen-space reflections can't show, and replace the glass "
			"cube reflections with the same. When on it OWNS every reflective pixel and the screen-space "
			"Reflections above are skipped entirely - no need to keep them on (Path Tracing turns them off). "
			"Needs the Vulkan backend + ray-query (RT) hardware. The Path Tracing tier; detailed tuning "
			"(temporal, jitter, blur, reflect monsters) is in the Debug tab's Ray-Traced Reflections group." );
		ImGui::EndDisabled();
		// explain the gray-out when the RT scene is the only thing missing (VK is already there)
		if ( rtIsVulkan && !rtSceneOn ) {
			ImGui::TextDisabled( "  (needs Ray-Traced Sun Shadows on to build the RT scene)" );
		}

		// RT reflection resolution (r_rtReflResScale): the RT trace runs at this fraction of the screen and is
		// temporally upscaled back to full. Discrete INTEGER-DIVISOR stops (Full/Half/Quarter) - fractional
		// ratios strobe the reflection. Sits under the RT toggle; the biggest single saving on the RT
		// reflection pass.
		const float rtResStops[]  = { 0.25f, 0.5f, 1.0f };
		const char *rtResLabels[] = { "Quarter (1/4)", "Half (1/2)", "Full" };
		ImGui::BeginDisabled( !rtIsVulkan || !rtSceneOn || !r_rtReflections.GetBool() );
		CvarStopsSliderF( "RT Reflection Resolution", r_rtReflResScale, rtResStops, rtResLabels, IM_ARRAYSIZE( rtResStops ) );
		AddTooltip( "Resolution the ray-traced reflection is traced at, then temporally upscaled to full. "
			"Half is ~4x cheaper on the reflection pass with a modest softening; Quarter cheaper still. "
			"The biggest single lever on RT reflection cost." );
		ImGui::EndDisabled();
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

		CvarSliderI( "Spot / Projected Map Resolution", r_shadowMapSize, 256, 4096, "Resolution of the 2D depth map used only by projected / spot lights. "
			"Point (omni) lights are the bulk of Doom 3's shadows and use the separate Point / "
			"Cube Map Resolution slider below instead — so on maps with no shadow-casting "
			"spotlight this has no visible effect (r_shadowMapDebug 1 shows the '2D-mapped' "
			"count). Higher = sharper spot-shadow edges, more memory." );

		CvarSliderI( "Point / Cube Map Resolution", r_shadowMapPointSize, 128, 4096, "Per-face resolution of the point-light cube map — this is the one that "
			"affects most Doom 3 shadows. Six faces cover the whole sphere, so it can be lower "
			"than the spot/projected map for similar quality. Higher = sharper, more memory and "
			"fill (6 faces). Pair with Cube Edge Softness (Developer tab) to smooth edges without "
			"raising resolution." );

		CvarCheckbox( "Perforated Casters (grates/fences)", r_shadowMapPerforated, "Let alpha-tested grates, fences and foliage cast real punched-out "
			"shadows. In vanilla these are flagged noShadows because stencil volumes can't "
			"perforate, so they cast nothing (or a hand-faked shadow). Shadow maps can cut "
			"the holes, so this enables their true shadow. Turn off to keep vanilla behaviour." );

		CvarCheckbox( "View Weapon Casts Shadows", r_shadowMapViewWeapon, "Let the first-person weapon (and the player's arms) cast shadow-map "
			"shadows. Off (default) keeps the weapon out of the map, so it never throws a "
			"gun-shaped shadow onto nearby floors/walls (a few weapons — chainsaw chain, "
			"plasmagun canister — would otherwise cast because id didn't flag them noShadows). "
			"On lets it cast: the gun self-shadows but also shadows the world, since one "
			"shadow map can't separate the two." );

		// Point (omni) lights use a 6-face cube map — the bulk of Doom 3's shadows.
		// Budgeting how many get one keeps the cost bounded in busy rooms; the rest
		// keep their vanilla stencil shadows, so there is no fidelity loss.
		ImGui::SeparatorText( "Point Lights" );

		CvarSliderI( "Point Light Budget", r_shadowMapPointLimit, 0, 128, "How many point lights get a soft cube shadow map per view, chosen by "
			"on-screen importance. While shadow mapping is on, out-of-budget point lights "
			"render unshadowed (stencil shadows are fully off). Lower = faster in crowded "
			"scenes. 0 = all point lights (default; most consistent, slowest)." );

		// Ray-traced shadows (Vulkan + RT hardware). Greyed out on the legacy/GL3 backend
		// and on non-RT GPUs, where the cvars are inert — a checkbox you can't toggle there
		// reads clearer than one that silently does nothing. This is the Path Tracing tier.
		const bool rtCapable = R_SupportsRayTracing();
		ImGui::SeparatorText( rtCapable ? "Ray Tracing (RTX)" : "Ray Tracing (RTX) - needs Vulkan + RT GPU" );
		ImGui::BeginDisabled( !rtCapable );

		CvarCheckbox( "RT Sun Shadows", r_rtSunShadows, "Trace the sun's shadow with one ray per pixel instead of sampling the "
			"fitted sun shadow map: pixel-exact at any distance, with no range cap and no "
			"map-resolution aliasing. Serves parallel suns and the big oversize omnis the "
			"sun-map fit declines. Vulkan + RT hardware only. Enabling it builds the persistent "
			"ray-tracing scene for the map." );

		CvarCheckbox( "RT Moving-Light Shadows", r_rtMovingLights, "Trace shadows for point lights that are moving (muzzle flashes, projectiles, "
			"monster-carried lights) instead of re-rendering their 6-face cube map every frame - "
			"the one case the shadow cache can never help, since a moving light must re-render "
			"each frame. Still lights keep the cached cube map; a light that stops re-caches after "
			"a moment. Vulkan + RT hardware only." );

		// r_rtAllLights: preset-owned (on at Path Tracing), so flipping it by hand reads "Custom"
		CvarCheckbox( "RT Shadows for All Lights", r_rtAllLights, "Every shadow-casting light gets ray-traced shadows - one hard ray per lit pixel "
			"toward the light, like the two options above - and no shadow maps are rendered at all. "
			"Pixel-exact shadows from every light, no map resolution or bias artefacts. Heavier in "
			"rooms with many lights: a still light's cached shadow map is nearly free, a ray is paid "
			"every frame. Changes "
			"the look of shadows: hard-edged unless Soften RT Shadows is on, which then softens all "
			"of them. Vulkan + RT hardware only." );

		// r_rtPerforatedShadows: not preset-owned (default on; it only matters while a light is ray-traced)
		ImGui::BeginDisabled( !( r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() || r_rtAllLights.GetBool() ) );
		bool rtPerf = r_rtPerforatedShadows.GetBool();
		if ( ImGui::Checkbox( "RT Grate & Fence Shadows", &rtPerf ) ) {
			r_rtPerforatedShadows.SetBool( rtPerf );
		}
		ImGui::EndDisabled();
		AddTooltip( "Grates, fences and other see-through surfaces cast shadows under ray-traced lights "
			"too: the shadow ray reads the grate's texture where it crosses one, so light passes through "
			"the holes. How dark these shadows get follows the Perforated Shadow Strength setting of the "
			"shadow maps. Off = grates cast nothing under ray-traced lights (slightly cheaper)." );

		// r_rtShadowBlur (docs/rtx-shadow-blur.md): a toggle ON TOP of the two ray-traced shadow
		// routes above; preset-owned (on at Path Tracing), so flipping it by hand reads "Custom"
		ImGui::BeginDisabled( !( r_rtSunShadows.GetBool() || r_rtMovingLights.GetBool() || r_rtAllLights.GetBool() ) );
		CvarCheckbox( "Soften RT Shadows", r_rtShadowBlur, "Blurs the ray-traced shadows above (RT Sun / RT Moving-Light / All Lights) in screen space: "
			"the edge stays sharp where an object touches the surface it shadows and softens as "
			"the gap grows, by up to 24 pixels. Same rays, no noise and nothing accumulated over "
			"frames - off gives back exactly the hard ray-traced shadows. Changes the look of "
			"shadows (the stock game's are hard-edged). Needs SSAO, RTAO or Motion Vectors on "
			"(their depth/normal prepass). Other lights are not affected. Blur Intensity and the "
			"falloff curve are tuned in Debugging -> RT Shadows." );

		// r_rtShadowBlurStagger: preset-owned (on at Path Tracing), so flipping it by hand reads "Custom"
		ImGui::BeginDisabled( !r_rtShadowBlur.GetBool() );
		CvarCheckbox( "Refresh Distant Soft Shadows Less Often", r_rtShadowBlurStagger, "Performance option for Soften RT Shadows: lights close to you still refresh their "
			"soft shadows every frame, farther ones every 2nd frame, the farthest every 3rd - set "
			"the two distances in Debugging -> RT Shadows. Reused shadows are re-projected to the current camera, and "
			"pixels they cannot cover get a hard-edged ray for that frame - so watch for brief hard "
			"edges or slightly late shadows from moving things. Stands down automatically during "
			"scripted cutscenes (every light refreshes each frame), where camera cuts would otherwise "
			"make reused shadows flicker. Uses ~15 MB of VRAM per staggered light at 1440p." );
		ImGui::EndDisabled();	// blur off
		ImGui::EndDisabled();

		ImGui::EndDisabled();

		EndSettingsGroup();
	}
}

// Display colour controls: Brightness / Gamma and their HDR counterparts (Exposure / HDR Gamma).
// An active HDR tonemap curve (r_hdr + r_hdrTonemap>=1) is the frame's display transform, so
// RB_RHI_GammaBrightness / the VK resolve skip r_gamma/r_brightness then - grey the LDR pair out
// in that case and enable the HDR pair instead (they only shape the image with a curve on).
static void DrawDisplayColorControls()
{
	const bool hdrCurve = r_hdr.GetBool() && cvarSystem->GetCVarInteger( "r_hdrTonemap" ) >= 1;

	// LDR Brightness + Gamma (displayColorOptions[0..1]) - live; bypassed under an active HDR curve.
	ImGui::BeginDisabled( hdrCurve );
	DrawOptions( &displayColorOptions[0], 2 );
	ImGui::EndDisabled();

	// HDR Exposure + Gamma (moved here from Post-Processing): the HDR equivalents of the two sliders
	// above. They only shape the image with a tonemap curve on, so they enable exactly when the LDR
	// pair greys out.
	ImGui::BeginDisabled( !hdrCurve );
	float exposure = cvarSystem->GetCVarFloat( "r_hdrExposure" );
	if ( ImGui::SliderFloat( "Exposure", &exposure, 0.1f, 8.0f, "%.2f" ) ) {
		cvarSystem->SetCVarFloat( "r_hdrExposure", exposure );
	}
	AddTooltip( "r_hdrExposure: linear exposure multiplier applied before the tonemap curve.\n"
		"Raises or lowers the scene brightness feeding the curve; default 1.5 (tuned for DUDE). "
		"Needs HDR + a tonemap curve (see Post-Processing)." );
	float hgamma = cvarSystem->GetCVarFloat( "r_hdrGamma" );
	if ( ImGui::SliderFloat( "HDR Gamma", &hgamma, 0.5f, 2.5f, "%.2f" ) ) {
		cvarSystem->SetCVarFloat( "r_hdrGamma", hgamma );
	}
	AddTooltip( "r_hdrGamma: display gamma applied on top of the tonemap curve (the normal Gamma\n"
		"above is bypassed while a curve is active). >1 lifts shadows/midtones for a softer image;\n"
		"<1 deepens contrast; 1 = off. Needs HDR + a tonemap curve (see Post-Processing)." );
	ImGui::EndDisabled();

	// r_gammaInShader (displayColorOptions[2]) - technical toggle, always available.
	DrawOptions( &displayColorOptions[2], 1 );
}

static void DrawGraphicsMenu()
{
	ImGui::Spacing();

	// --- Display / renderer settings (work on every backend). These stay above and
	// outside the enhancement-only block below. ---
	DrawRendererBackend();
	DrawOpenGLInfo();

	// Display / windowing / output settings - a collapsible "burger" group like the enhancement
	// groups below. Renderer Backend stays pinned above it.
	if ( BeginSettingsGroup( "Display" ) ) {
		DrawDisplaySettings();
		// Brightness / Gamma plus their HDR counterparts (Exposure / HDR Gamma); the LDR pair greys
		// out under an active HDR tonemap curve and the HDR pair takes over (DrawDisplayColorControls).
		DrawDisplayColorControls();
		EndSettingsGroup();
	}

	// DUDE FSR2 sub-native upscaling (docs/fsr-temporal-pipeline.md): the 3D scene renders at a
	// fraction of the display size and FSR2 upscales it, so every screen-space and ray-traced pass
	// pays for the render pixels only; the HUD, film grain and chromatic aberration stay at display
	// resolution. Vulkan + FSR2 TAA only -- the group greys out otherwise and says why. Takes effect
	// live (no restart): the scene buffer and the FSR2 context rebuild on the next frame.
	if ( BeginSettingsGroup( "Upscaling" ) ) {
		const bool isVulkan = glConfig.rhiBackend && !glConfig.coreProfile;
		const bool fsrOn = isVulkan && r_fsr.GetBool();
		if ( !isVulkan ) {
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ), "Needs the Vulkan renderer backend (top of this tab)." );
		} else if ( !fsrOn ) {
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ), "Select FSR2 TAA under Antialiasing to enable upscaling." );
		}
		ImGui::BeginDisabled( !fsrOn );

		// The quality levels are render scales (the AMD naming, plus Ultra Quality at 75%); any
		// other value reads back as Custom, and picking Custom keeps the current scale and frees
		// the slider. Detection is per frame from the live cvar, so console changes show up too.
		static const char* qualityItems =
			"Native AA (100%)\0Ultra Quality (75%)\0Quality (67%)\0Balanced (59%)\0Performance (50%)\0Custom\0";
		static const float qualityScales[] = { 1.0f, 0.75f, 0.67f, 0.59f, 0.5f };
		static bool customMode = false;
		const float scale = r_fsrScale.GetFloat();
		int detected = 5;		// Custom
		for ( int i = 0; i < 5; i++ ) {
			if ( idMath::Fabs( scale - qualityScales[i] ) < 0.005f ) {
				detected = i;
				break;
			}
		}
		int sel = customMode ? 5 : detected;
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::Combo( "FSR2 Quality", &sel, qualityItems ) ) {
			if ( sel < 5 ) {
				customMode = false;
				r_fsrScale.SetFloat( qualityScales[sel] );
			} else {
				customMode = true;
			}
		}
		AddTooltip( "How much of the display resolution the 3D scene is rendered at before FSR2 "
			"upscales it. Native AA renders every pixel (FSR2 acts as temporal antialiasing only). "
			"Ultra Quality 75%, Quality 67%, Balanced 59% and Performance 50% render fewer pixels - "
			"at 67% roughly 45% of them - and every screen-space and ray-traced pass (reflections, "
			"GI, ambient occlusion, volumetrics, soft shadows) gets cheaper by about that much, so "
			"this is the biggest single frame-rate lever on the Path Tracing tier. The HUD, "
			"menus, film grain and chromatic aberration always stay at full display resolution. "
			"Custom keeps whatever scale is set and unlocks the slider below. (r_fsrScale)" );

		ImGui::BeginDisabled( !( customMode || detected == 5 ) );
		float custom = scale;
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::SliderFloat( "Render Scale", &custom, 0.5f, 1.0f, "%.2f" ) ) {
			r_fsrScale.SetFloat( custom );
		}
		AddTooltip( "The render scale directly, 0.50 - 1.00 of the display size per axis. "
			"Only editable with Custom selected above. (r_fsrScale)" );
		ImGui::EndDisabled();	// custom slider gate

		// what the scene actually renders at, for the current window
		{
			const int dw = glConfig.vidWidth, dh = glConfig.vidHeight;
			const float s = idMath::ClampFloat( 0.5f, 1.0f, scale );
			const int rw = ( s < 0.999f ) ? Max( 1, (int)floorf( (float)dw * s + 0.5f ) ) : dw;
			const int rh = ( s < 0.999f ) ? Max( 1, (int)floorf( (float)dh * s + 0.5f ) ) : dh;
			if ( rw == dw && rh == dh ) {
				ImGui::TextDisabled( "Renders at %d x %d (native)", dw, dh );
			} else {
				ImGui::TextDisabled( "Renders at %d x %d, upscaled to %d x %d", rw, rh, dw, dh );
			}
		}

		// The sharpening knob mirrored here for convenience: it is the SAME setting as under
		// Antialiasing (both read and write r_fsrSharpness), because FSR2's RCAS pass runs on the
		// upscaled image in every mode -- a lower render scale softens more, so this is the knob
		// to reach for right after picking a quality level.
		ImGui::Spacing();
		float sharp = r_fsrSharpness.GetFloat();
		ImGui::SetNextItemWidth( 220.0f );
		if ( ImGui::SliderFloat( "RCAS Sharpness", &sharp, 0.0f, 1.0f, "%.2f" ) ) {
			r_fsrSharpness.SetFloat( sharp );
		}
		AddTooltip( "FSR2's built-in contrast-adaptive sharpening (RCAS), applied to the upscaled "
			"image at display resolution in every mode - the same control as under Antialiasing. "
			"Lower render scales soften more, so you may want a little more here at 50-67%. "
			"0 disables the pass. (r_fsrSharpness)" );

		ImGui::EndDisabled();	// FSR2-on gate
		EndSettingsGroup();
	}

	// Texture Options + the shared Apply / Reset sit right under Display: the other "every backend"
	// video settings that need a renderer restart, kept together at the top of the tab.
	if ( BeginSettingsGroup( "Texture Options" ) ) {
		DrawTextureOptions();
		EndSettingsGroup();
	}

	// Shared Apply / Reset for the video settings that need a renderer restart (resolution, window
	// mode, fullscreen-desktop, textures, MSAA). Frame Rate Cap and Vertical Sync take effect live.
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

	ImGui::Spacing();

	const bool supported = R_BackendSupportsEnhancements();
	if ( !supported ) {
		ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ),
			"These settings don't apply on the current (legacy) backend." );
		ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ),
			"Choose OpenGL 3.3 as the Renderer Backend at the top of this tab to enable them." );
		ImGui::Spacing();
	}

	// One-click quality presets for the whole enhancement suite. Gated to the RHI backends (the
	// presets only touch enhancement cvars). The combo is the target; the status line reflects the
	// live cvars, so hand-tweaking any slider below reads back as "Custom".
	ImGui::BeginDisabled( !supported );
	ImGui::SeparatorText( "Quality Preset" );
	{
		static int selPreset = -1;
		static int selRtx    = -1;    // pending RTX axis (RtMode); synced from detection
		static int lastDetected = -2; // -2 = "not seen yet" -> forces a first-frame sync
		static int lastDetectedRt = -2;

		int detectedRt = RTX_OFF;
		const int detected = DetectEnhancementPreset( &detectedRt );
		if ( selPreset < 0 ) {
			selPreset = ( detected >= 0 ) ? detected : PRESET_HIGH;
		}
		if ( selRtx < 0 ) {
			selRtx = detectedRt;
		}
		// Re-sync the pending selectors to the live state whenever detection moves on either axis
		// (a preset Apply, or a hand-toggle of a tier/RT cvar), so the combo + RTX tickbox keep
		// showing reality. A hand-tweak to Custom (detected -1) leaves the last choice in place.
		if ( detected >= 0 && ( detected != lastDetected || detectedRt != lastDetectedRt ) ) {
			selPreset = detected;
			selRtx    = detectedRt;
		}
		lastDetected   = detected;
		lastDetectedRt = detectedRt;

		// Axis 1: the raster quality tier (every GL3.3 card).
		ImGui::SetNextItemWidth( 220.0f );
		ImGui::Combo( "##enhPreset", &selPreset,
			"Potato\0Low\0Medium\0High\0Ultra\0Nightmare\0" );

		// Axis 2: ray tracing, orthogonal to the tier. The tickbox sits immediately to the right of
		// the dropdown; it is greyed (and forced off) unless the device is RT-capable (VK + ray-query)
		// and the chosen tier actually gains RT features (Medium and up - see rtAugment[]).
		const bool rtCapable = R_SupportsRayTracing();
		const bool rtTierHasRT = ( selPreset >= PRESET_MEDIUM );
		ImGui::SameLine();
		ImGui::BeginDisabled( !rtCapable || !rtTierHasRT );
		int rtSel = idMath::ClampInt( RTX_OFF, RTX_PATHTRACED, selRtx );
		ImGui::SetNextItemWidth( 160.0f );
		if ( ImGui::Combo( "##enhRtx", &rtSel, "RTX Off\0RTX On\0Path Tracing\0" ) ) {
			selRtx = rtSel;
		}
		ImGui::EndDisabled();
		AddTooltip( rtCapable
			? "Ray tracing, layered on top of the quality tier at left: Medium and High add ray-traced "
			  "ambient occlusion (at rising resolution; High also adds ray-traced shadows), Ultra swaps "
			  "the AO for one-bounce ray-traced global illumination plus RT reflections, and Nightmare "
			  "adds soft RT shadows on every light and volumetric lighting. 'Path Tracing' (dude_rtx 2) "
			  "goes further: ReSTIR direct lighting lights the whole scene with one resampled light and one "
			  "shadow ray per pixel (the per-light forward draws are skipped), with the GI bounce folded into "
			  "the same denoise and a world-space irradiance cache serving the bounce for steadier frame pacing. "
			  "Independent of the tier, so it never changes the raster look. Needs the "
			  "Vulkan backend and ray-query (RTX) hardware."
			: "Ray tracing needs the Vulkan backend and ray-query (RTX) hardware - unavailable on this "
			  "GPU / backend, so the quality tiers run raster-only." );

		ImGui::SameLine();
		if ( ImGui::Button( "Apply Preset" ) ) {
			const int applyRtx = ( rtCapable && rtTierHasRT ) ? selRtx : RTX_OFF;
			ApplyEnhancementPreset( selPreset, applyRtx );
			dude_preset.SetInteger( selPreset );
			dude_rtx.SetInteger( applyRtx );
			selRtx       = applyRtx;
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

		// Status line: raster tier + whether RTX is live. "Nightmare + RTX" is the old Path Tracing.
		if ( detected < 0 ) {
			ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.2f, 1.0f ), "Current: Custom (hand-tuned)" );
		} else if ( detectedRt != RTX_OFF ) {
			ImGui::TextDisabled( "Current: %s + RTX", enhancementPresets[detected].name );
		} else {
			ImGui::TextDisabled( "Current: %s", enhancementPresets[detected].name );
		}
	}
	ImGui::EndDisabled();
	ImGui::Spacing();

	// Groups below the preset, in the user-set on-screen order. Anisotropic Filtering and Antialiasing
	// work on every backend (legacy included), so they stay OUTSIDE the enhancement gate; the rest are
	// enhancement-only and grey out on the legacy backend.
	if ( BeginSettingsGroup( "Anisotropic Filtering" ) ) {
		DrawOptions( anisoOptions, IM_ARRAYSIZE( anisoOptions ) );
		EndSettingsGroup();
	}
	DrawGroup_Antialiasing();

	ImGui::BeginDisabled( !supported );
	DrawEnhGroup_SurfaceRelief();
	DrawEnhGroup_Tessellation();
	DrawEnhGroup_Shadows();
	DrawEnhGroup_AmbientOcclusion();
	DrawEnhGroup_Lighting();
	DrawEnhGroup_Reflections();
	DrawEnhGroup_Particles();
	DrawEnhGroup_PostProcess();
	ImGui::EndDisabled();

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

	CvarSliderI( "Wireframe", r_showTris, 0, 3, "r_showTris: 0 = off, 1 = visible triangles, 2 = all front-facing, 3 = all." );

	bool lights = !r_skipInteractions.GetBool();
	if ( ImGui::Checkbox( "Lights (dynamic lighting)", &lights ) ) {
		r_skipInteractions.SetBool( !lights );
	}
	AddTooltip( "Off skips all light/surface interaction drawing (r_skipInteractions); the scene keeps "
		"only its ambient/emissive passes." );

	CvarCheckbox( "Stencil Shadows", r_shadows, "Toggles stencil shadow volumes (the vanilla technique) and self-shadowing (r_shadows). "
		"On opengl3/Vulkan with Shadow Mapping enabled, shadows come from the shadow maps below instead, "
		"so this only affects the stencil path." );

	// Vulkan crash logging (opt-in) - writes a dudelog-crash-YYYYMMDD-HHMMSS.txt next to dudelog.txt
	// when a VK_ERROR_DEVICE_LOST is detected by the backend. Default OFF.
	bool vkCrashLog = cvarSystem->GetCVarBool( "r_vkCrashLogging" );
	if ( ImGui::Checkbox( "Enable Vulkan Crash Logging (write crash dumps)", &vkCrashLog ) ) {
		cvarSystem->SetCVarBool( "r_vkCrashLogging", vkCrashLog );
	}
	AddTooltip( "When enabled, writes a dudelog-crash-YYYYMMDD-HHMMSS.txt on device-lost events. Enable only when debugging driver resets." );

	// Deep-dive debug HUD overlay (Vulkan only): live GPU clock/temp/power, VRAM and validation errors.
	bool vkDebugHud = cvarSystem->GetCVarBool( "r_vkDebugHud" );
	if ( ImGui::Checkbox( "Show Vulkan Deep-Dive HUD (GPU clock/temp/VRAM/errors)", &vkDebugHud ) ) {
		cvarSystem->SetCVarBool( "r_vkDebugHud", vkDebugHud );
	}
	AddTooltip( "Always-on overlay with live GPU clock/temp/power (NVIDIA), VRAM usage and validation-error count. "
		"The clock turns amber/red as it climbs toward the boost range — useful for telling a hardware/driver reset apart from a code fault." );

	// Manual renderer restart: explicit user-driven restart is safer than an automatic in-process device re-create.
	if ( ImGui::Button( "Restart Renderer (manual vid_restart)" ) ) {
		cmdSystem->BufferCommandText( CMD_EXEC_APPEND, "vid_restart\n" );
	}
	AddTooltip( "Explicitly restart the renderer. Use this to recover after a device-lost or to apply backend changes." );

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
	CvarSliderF( "Tessellation Min Triangle Size", r_tessMinEdge, 0.0f, 8.0f, "%.2f", "r_tessMinEdge: triangles with edges finer than this (world units) stay flat, so dense "
		"detail (eye-sockets, faces) doesn't over-inflate under PN while larger silhouette triangles still "
		"smooth. Raise if faces/fine features distort; lower to smooth more aggressively. Vulkan only." );

	CvarCheckbox( "Log Tessellated Materials", r_tessDebug, "r_tessDebug: print each material name accepted for tessellation once (with model type and "
		"entity index) to the console, so a mis-tessellated surface can be identified. Vulkan only." );

	ImGui::EndDisabled();
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_SkinSSS()
{
	// DUDE skin / flesh subsurface scattering (docs/skin-shading.md). The on/off lives in the
	// Graphics tab (Lighting); these are the two live-tuning knobs. Both are per-draw uniforms, so
	// they apply instantly -- dial them in-game and report the good ones back.
	if ( BeginSettingsGroup( "Skin Subsurface Scattering" ) ) {

	if ( r_skinShading.GetFloat() <= 0.0f ) {
		ImGui::TextDisabled( "Skin SSS is off - enable \"Skin Subsurface Scattering (SSS)\" in the Graphics tab first." );
	}
	ImGui::BeginDisabled( r_skinShading.GetFloat() <= 0.0f );

	float sss = r_skinShading.GetFloat();
	if ( ImGui::SliderFloat( "Skin SSS Strength", &sss, 0.0f, 1.0f, "%.2f" ) ) {
		r_skinShading.SetFloat( idMath::ClampFloat( 0.0f, 1.0f, sss ) );
	}
	AddTooltip( "r_skinShading: how strongly skin and flesh scatter light under the surface, warming and "
		"softening the lighting terminator so faces stop reading as plastic. Doubles as the on/off, so "
		"dragging this to 0 also unticks it in the Graphics tab. Default 0.6. Preset-owned (Potato 0 = "
		"faithful floor, Low+ 0.6), so moving it reads as a Custom preset." );

	float reach = r_skinScatter.GetFloat();
	if ( ImGui::SliderFloat( "Skin Scatter Reach", &reach, 0.0f, 1.0f, "%.2f" ) ) {
		r_skinScatter.SetFloat( idMath::ClampFloat( 0.0f, 1.0f, reach ) );
	}
	AddTooltip( "r_skinScatter: how far light wraps around the terminator - higher = a softer, redder, "
		"wider glow, lower = tighter. The red channel's reach; green and blue derive shorter, which is "
		"what makes the falloff warm. Flesh scatters ~30% deeper than face skin. Default 0.32. NOT "
		"preset-owned, so it survives a preset change." );

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

	CvarSliderF( "Fallback Roughness", r_pbrRoughness, 0.03f, 1.0f, "%.2f", "r_pbrRoughness: roughness for materials without a pbr-table entry (0.03 = mirror, 1 = matte). "
		"0.58 matches the Blinn-Phong exponent-16 highlight width. Per-material values come from "
		"pbr/pbr_materials.cfg and override this." );

	CvarSliderF( "Specular Energy (dielectrics)", r_pbrSpecScale, 0.0f, 8.0f, "%.2f", "r_pbrSpecScale: artistic boost on the GGX lobe for dielectrics/painted surfaces (fades to 1 "
		"on bare metal, whose albedo-derived reflectance is already bright). The physical 4% dielectric "
		"reflectance reads dim in Doom 3's display-space shading; this compensates." );

	CvarSliderF( "Highlight Tightness (Toksvig base)", r_pbrToksvigBase, 0.0f, 0.6f, "%.2f", "r_pbrToksvigBase: how much normal-map variance is ignored before it widens (softens) the "
		"highlight. LOWER = softer highlights but strong anti-firefly filtering; HIGHER = tighter, punchier "
		"highlights but white pixel spikes creep back on panel seams. 0.2 = calibrated split." );

	CvarSliderF( "Highlight Ceiling (firefly clamp)", r_pbrFireflyClamp, 1.0f, 16.0f, "%.1f", "r_pbrFireflyClamp: upper bound on the specular lobe. Stops isolated texels clipping to pure "
		"white; also the ceiling that skin/tight highlights ride at. Raise for hotter cores (pairs well "
		"with HDR), lower to flatten everything." );

	CvarSliderF( "Metal Color Retention", r_pbrMetalDiffuse, 0.0f, 1.0f, "%.2f", "r_pbrMetalDiffuse: how much of a metal's painted albedo color survives. Physical PBR kills "
		"diffuse on metals (0) and pushes the color into reflections Doom 3 can't supply, so metals read dark "
		"and off-color; raise this to keep the asset look while the metallic specular still rides on top "
		"(1 = keep all, 0 = physical). Weighted by metalness, so it mainly affects bare metal." );

	CvarSliderF( "Metal Environment Glow", r_pbrEnvScale, 0.0f, 2.0f, "%.2f", "r_pbrEnvScale: metals reflect their surroundings, but Doom 3 has no environment probes — "
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

	CvarSliderF( "Reflection Intensity", r_ssrIntensity, 0.0f, 4.0f, "%.2f", "r_ssrIntensity: overall reflection strength on top of the physical Fresnel weight. "
		"1 = physical; below dampens, above exaggerates the mirror look." );

	CvarSliderF( "Roughness Cutoff", r_ssrMaxRoughness, 0.02f, 1.0f, "%.2f", "r_ssrMaxRoughness: surfaces rougher than this reflect nothing (fade starts at 70% of it). "
		"Reflections are currently sharp-only, so keep this low — rough painted walls showing crisp mirror "
		"images reads wrong. Raise it to let more of the world reflect." );

	// RR14: shared by SSR + RT reflections (like Intensity / Roughness Cutoff above), so it lives in this
	// group rather than duplicated in the RT group. Needs an AO buffer this view or it's inert.
	CvarSliderF( "Specular Occlusion", r_specOcclusion, 0.0f, 1.0f, "%.2f", "r_specOcclusion: dim reflections by ambient occlusion, roughness-aware — a polished floor "
		"or metal stops glowing in creases and under props/at contact, while a near-mirror is barely "
		"touched. Applies to both SSR and RT reflections. Needs Ambient Occlusion on (Graphics tab, or RTAO "
		"at Path Tracing); does nothing without it. Direct specular from lights is never occluded. "
		"0 = off, 0.7 = default." );

	CvarSliderI( "March Steps", r_ssrSteps, 4, 64, "r_ssrSteps: ray-march samples per pixel. More = longer, more complete reflections at "
		"higher GPU cost (watch r_gl3GpuTime). Fewer = cheaper, reflections cut off sooner." );

	CvarSliderF( "Max Distance", r_ssrMaxDistance, 64.0f, 4096.0f, "%.0f", "r_ssrMaxDistance: how far (world units) a reflection ray reaches. Rooms are ~256 units; "
		"1000 covers a large hall. Longer rays spread the same March Steps thinner." );

	CvarSliderF( "Hit Thickness", r_ssrThickness, 1.0f, 128.0f, "%.0f", "r_ssrThickness: assumed depth of surfaces when testing ray hits. Too low leaves gaps "
		"in reflections (rays slip behind geometry); too high smears streaks under railings and edges." );

	CvarSliderF( "Temporal Feedback##ssr", r_ssrTemporalFeedback, 0.0f, 0.97f, "%.2f", "r_ssrTemporalFeedback: fraction of reflection history kept per frame while Temporal "
		"Accumulation is on (Graphics tab). Higher = smoother, converges slower and can trail "
		"on fast motion; lower = grainier but snappier." );

	CvarSliderF( "Glass Probe Intensity", r_ssrGlassProbeScale, 0.0f, 4.0f, "%.2f", "r_ssrGlassProbeScale: brightness of the baked room cubemap on glass only "
		"(Graphics > Glass Reflections). Applied on top of the material's own reflection "
		"colour and the global Reflection Brightness; other reflective surfaces are unaffected." );

	ImGui::EndDisabled();
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_RTReflections()
{
	// Ray-traced reflections (docs/rtx-reflections.md). Traces the scene acceleration structure so
	// reflective floors/metal show the real room + monsters, including geometry off-screen or hidden from
	// the camera that screen-space SSR can't reach — the RT tier's reflection layer, which REPLACES the SSR
	// composite on reflective surfaces when it renders. Vulkan + ray-query (RT) hardware only. The RT
	// reflection has its OWN trace resolution (r_rtReflResScale, in the Graphics tab's Ray-Traced Reflections
	// group), unsnapped and temporally upscaled, separate from SSR's r_ssrResScale. Prerequisites (r_rtSunShadows
	// for the TLAS, r_pbr) live in their own groups; RT reflections no longer need r_ssr. This is the tuning.
	const bool isVulkan = glConfig.rhiBackend && !glConfig.coreProfile;
	if ( BeginSettingsGroup( "Ray-Traced Reflections" ) ) {

	if ( !isVulkan ) {
		ImGui::TextDisabled( "Requires the Vulkan backend + ray-query (RT) hardware." );
	}
	ImGui::BeginDisabled( !isVulkan );

	// The on/off switch now lives in the Graphics tab (its own Ray-Traced Reflections group); this group is the
	// RT-reflection tuning. Point there when it's off so the knobs below aren't a dead end.
	if ( !r_rtReflections.GetBool() ) {
		ImGui::TextDisabled( "RT Reflections are off - enable \"Ray-Traced Reflections\" in the Graphics tab." );
	}

	// prerequisite hints (only meaningful once RT reflections are switched on). RT no longer needs r_ssr.
	if ( r_rtReflections.GetBool() ) {
		if ( !r_rtSunShadows.GetBool() ) {
			ImGui::TextDisabled( "  (!) No RT scene - enable Ray-Traced Sun Shadows so the world/monster BLAS is built." );
		}
	}

	ImGui::BeginDisabled( !r_rtReflections.GetBool() );

	// (Trace resolution lives in the Graphics tab's Reflections group, Ray Tracing (RTX) section, with the
	// RT toggle - the user-facing quality control. This group is the fine tuning.)

	CvarCheckbox( "Temporal Upscale (r_rtReflTemporal)", r_rtReflTemporal, "r_rtReflTemporal: accumulate the reflection across frames and reconstruct it at full "
		"resolution (the ray-query trace itself stays at the SSR resolution for speed). Resolves the "
		"single-ray grain and the fractional-resolution shimmer. Leave on." );

	CvarSliderF( "Normal-Map Warp (r_rtReflBump)", r_rtReflBump, 0.0f, 1.0f, "%.2f", "r_rtReflBump: how much the reflection follows the surface normal map. 1 = full bump, "
		"warping over tile/grout relief exactly like SSR; 0 = a flat, glassy mirror. Lower it if a rougher "
		"surface sparkles." );

	CvarSliderF( "Ray Jitter (r_rtReflJitter)", r_rtReflJitter, 0.0f, 4.0f, "%.2f", "r_rtReflJitter: per-frame sub-pixel jitter of the reflection sample grid so the temporal "
		"upscale can reconstruct detail and the fractional-resolution beat averages out. Needs Temporal "
		"Upscale. 1 = one sample cell; higher = softer, lower = sharper but more residual shimmer." );

	CvarSliderF( "Blur (r_rtReflBlur)", r_rtReflBlur, 0.0f, 1.0f, "%.2f", "r_rtReflBlur: a small box blur of the reflection before compositing, to soften any "
		"residual single-pixel speckle. 0 = sharp." );

	bool animBlas = cvarSystem->GetCVarBool( "r_rtAnimBlas" );
	if ( ImGui::Checkbox( "Reflect Monsters (r_rtAnimBlas)", &animBlas ) ) {
		cvarSystem->SetCVarBool( "r_rtAnimBlas", animBlas );
	}
	AddTooltip( "r_rtAnimBlas: put animated monsters into the ray-tracing acceleration structure so they "
		"appear in reflections (and RT shadows). On by default - without it a monster shows as a black "
		"silhouette in reflections. Turning it off saves the per-frame monster BLAS build." );

	CvarSliderF( "Glass Intensity (r_rtReflGlassScale)", r_rtReflGlassScale, 0.0f, 4.0f, "%.2f", "r_rtReflGlassScale: brightness of the RT reflection on GLASS panes only - goggles/visors, "
		"the dropship cockpit. Those reflect a nearly-black cube in vanilla (that darkness is their authored "
		"glass reflectivity), scaled by its perceptual brightness so the pane reads as subtle glass rather than "
		"a mirror (windows stay shinier than goggles). Default 2 = the calibrated glass look, 1 = subtler, "
		"higher = shinier toward a mirror, 0 = clear pane. Other reflective surfaces (floors, metal) are unaffected." );

	ImGui::EndDisabled();	// r_rtReflections
	ImGui::EndDisabled();	// isVulkan
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_RTShadows()
{
	// Live tuning for r_rtShadowBlur (docs/rtx-shadow-blur.md). The on/off toggle lives in
	// Graphics -> Shadows -> Ray Tracing ("Soften RT Shadows"); these shape the blur.
	const bool rtCapable = R_SupportsRayTracing();
	if ( BeginSettingsGroup( "RT Shadows" ) ) {

	if ( !rtCapable ) {
		ImGui::TextDisabled( "Requires the Vulkan backend + ray-query (RT) hardware." );
	}
	ImGui::BeginDisabled( !rtCapable );
	if ( !r_rtShadowBlur.GetBool() ) {
		ImGui::TextDisabled( "Soften RT Shadows is off - enable it in Graphics -> Shadows -> Ray Tracing (RTX)." );
	}
	ImGui::BeginDisabled( !r_rtShadowBlur.GetBool() );

	CvarSliderF( "Blur Intensity", r_rtShadowBlurIntensity, 0.0f, 4.0f, "%.2f", "How strongly the ray-traced shadows are blurred. The blur still follows the "
		"scene - none where an object touches the surface, more as the gap grows - this scales "
		"all of it. It tops out at 24 pixels, so high values mostly soften the edges near "
		"contact further. 0 = hard shadows. Default 1.5." );

	CvarSliderF( "Blur Falloff Curve", r_rtShadowBlurCurve, 0.25f, 4.0f, "%.2f", "How quickly a shadow softens as it gets further from the object casting it. "
		"1 = the natural growth of a real light. Lower = soft almost straight away, then "
		"levelling off (a more even blur). Higher = crisp for longer near the object, then "
		"widening fast. Blur Intensity sets how much; this sets where. Default 1.45." );

	CvarSliderF( "Skip Below Penumbra", r_rtShadowBlurMinPenumbra, 0.0f, 8.0f, "%.1f px", "Performance: a distant or small light casts a shadow edge finer than this many "
		"screen pixels, so softening it would be invisible - skip its blur entirely and keep the "
		"crisp hard ray (the whole per-light ray + blur chain is saved). Higher skips more lights "
		"for more FPS; parallel suns are never skipped. 0 = blur every ray-served light. "
		"Preset defaults: 4 at Ultra(RTX), 2 at Nightmare(RTX). Watch rtblur.ray's calls/frame in r_vkGpuTime 2." );

	ImGui::BeginDisabled( !r_rtShadowBlurStagger.GetBool() );
	if ( !r_rtShadowBlurStagger.GetBool() ) {
		ImGui::TextDisabled( "Refresh Distant Soft Shadows Less Often is off (Graphics -> Shadows -> Ray Tracing)." );
	}
	CvarSliderF( "Half Rate Beyond", r_rtShadowBlurStaggerNear, 0.0f, 2048.0f, "%.0f units", "Lights whose centre is farther from you than this refresh their soft shadows every "
		"2nd frame (nearer ones every frame). For scale: the player is ~74 units tall, a typical "
		"room 300-600 across. 0 = every light at half rate or less. Default 192." );
	CvarSliderF( "Third Rate Beyond", r_rtShadowBlurStaggerFar, 0.0f, 4096.0f, "%.0f units", "Lights whose centre is farther from you than this refresh every 3rd frame. "
		"0 = every light at a third of the rate. Default 380." );
	ImGui::EndDisabled();	// r_rtShadowBlurStagger

	ImGui::EndDisabled();	// r_rtShadowBlur
	ImGui::EndDisabled();	// rtCapable
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_RTGI()
{
	// Live tuning + debug view for ray-traced one-bounce GI (docs/rtx-rtgi.md). The on/off toggle is
	// preset-driven (Graphics -> Ambient Occlusion -> Ray Tracing); every r_rtgi* knob is gathered here
	// for A/B. Several also appear in the Graphics tab RTGI section - same live cvars, edited either place.
	const bool rtCapable = R_SupportsRayTracing();
	if ( BeginSettingsGroup( "RT Global Illumination" ) ) {

	if ( !rtCapable ) {
		ImGui::TextDisabled( "Requires the Vulkan backend + ray-query (RT) hardware." );
	}
	ImGui::BeginDisabled( !rtCapable );
	if ( !r_rtgi.GetBool() ) {
		ImGui::TextDisabled( "RT Global Illumination is off - enable it in Graphics -> Ambient Occlusion -> Ray Tracing (RTX)." );
	}
	ImGui::BeginDisabled( !r_rtgi.GetBool() );

	// --- Debug view (the RTGI buffer blitted over the scene) ---
	ImGui::SeparatorText( "Debug View" );
	{
		int dbg = idMath::ClampInt( 0, 2, r_rtgiDebug.GetInteger() );
		ImGui::SetNextItemWidth( 240.0f );
		if ( ImGui::Combo( "Buffer##rtgiDebug", &dbg, "Off (composited into scene)\0Raw radiance gather\0NRD-denoised\0" ) ) {
			r_rtgiDebug.SetInteger( dbg );
		}
		AddTooltip( "Blit the RTGI buffer over the finished frame instead of compositing it into the scene "
			"(the composite is suppressed while this is on). 'Raw radiance gather' = the noisy 1-ray/px "
			"input before denoising; 'NRD-denoised' = what actually lights the scene (falls back to raw if "
			"the denoise did not run). YCoCg-decoded; HDR above 1 clips on the overlay, so read structure "
			"and noise, not absolute brightness. (r_rtgiDebug)" );
	}

	// --- Resolution ---
	ImGui::SeparatorText( "Resolution" );
	{
		const float rtgiResStops[]  = { 0.25f, 0.5f, 0.667f, 0.75f, 1.0f };
		const char *rtgiResLabels[] = { "Quarter (1/4)", "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Full" };
		CvarStopsSliderF( "Gather Resolution", r_rtgiResScale, rtgiResStops, rtgiResLabels, IM_ARRAYSIZE( rtgiResStops ),
			"Fraction of the screen the GI gather (and its NRD denoise) runs at, bilinearly upscaled. Bounce "
			"light is very low-frequency so this is nearly free of visible quality but the single biggest cost "
			"lever (~1/scale^2). Default 3/4. (r_rtgiResScale)" );
		CvarCheckbox( "Checkerboard Gather", r_rtgiCheckerboard, "Trace a full-res horizontal checkerboard (half the pixels, "
			"alternating each frame) and let NRD reconstruct the rest, instead of a uniform downscale - keeps silhouette edges "
			"sharper at ~half the rays. Composes with Gather Resolution (traces half of those pixels); sharpest at Full. "
			"(r_rtgiCheckerboard)" );
		CvarCheckbox( "Bilateral Upscale", r_rtgiBilateralUpscale, "Upscale the reduced-res GI with a depth+normal-aware filter "
			"instead of plain bilinear, so bounce light stops bleeding across silhouettes and creases - sharper GI at edges for "
			"a few extra taps. Off = the old bilinear look. (r_rtgiBilateralUpscale)" );
	}

	// --- Noise suppression ---
	ImGui::SeparatorText( "Noise Suppression" );
	{
		CvarSliderI( "History Length", r_rtgiHistory, 2, 63, "Max frames NRD's ReBLUR accumulates for the GI radiance. The "
			"1-spp gather needs temporal accumulation to look smooth: too short and single pixels 'fry' (noise/flicker), too "
			"long and a switched-off light or stale bounce lingers. ~6-12 is the balance; higher = smoother but laggier. "
			"NRD's own default is 30. Overridden by History (ms) when that is > 0. (r_rtgiHistory)" );
		CvarSliderF( "History (ms)", r_rtgiHistoryMs, 0.0f, 2000.0f, "%.0f", "The GI history length in MILLISECONDS instead of "
			"frames: > 0 converts to frames from a 0.2 s low-passed frame rate, so the accumulation window is the same TIME at "
			"any frame rate (the fast history scales with it). Overrides History Length while > 0. 0 = use History Length in "
			"frames. (r_rtgiHistoryMs)" );
		CvarSliderF( "Fast-History Clamp", r_rtgiFastClamp, 1.0f, 3.0f, "%.1f", "NRD ReBLUR fastHistoryClampingSigmaScale - how "
			"tightly the slow (smooth) history is clamped to the fast (responsive) one. 2.0 = NRD default; the NRD sample ships "
			"1.5 ('works well even for dirty signals'). Lower = less ghosting / lag but more flicker on the 1-spp bounce, higher "
			"= smoother but laggier on a change. (r_rtgiFastClamp)" );
		CvarSliderF( "History Confidence", r_rtgiConfidence, 0.0f, 16.0f, "%.1f", "Per-pixel history rejection where this frame's "
			"bounce disagrees with last frame's reprojected result (a lit edge moving, a light toggling) - kills edge ghosting/"
			"trailing without the flat-area dimming a global antilag causes. Higher = more aggressive (sharper edges, more noise "
			"on them); 0 = off (NRD's built-in antilag only). (r_rtgiConfidence)" );
		CvarSliderF( "Emissive Clamp", r_rtgiEmissiveClamp, 0.0f, 16.0f, "%.2f", "Ceiling on a single bounce ray's emissive "
			"contribution before denoising. Small bright emitters are the worst 1-spp input (hit = big spike, miss = zero), and "
			"that variance denoises into grain/colour blotches; clamping the peak cuts it. Lower = cleaner/dimmer glow bleed, "
			"higher = brighter/noisier; ~16 effectively disables it. Default 0.15 (lowered from 0.38 to tame strobing light "
			"fixtures scattering the bounce into 'raindrops'). (r_rtgiEmissiveClamp)" );
	}

	// --- Appearance / strength ---
	ImGui::SeparatorText( "Appearance" );
	{
		CvarSliderF( "Intensity", r_rtgiIntensity, 0.0f, 8.0f, "%.2f", "Apply strength: multiplier on the bounced radiance added "
			"into the scene. 1 = the one-bounce irradiance as-is; raise for punchier colour bleed, lower for a subtle fill. "
			"Default 1.33. (r_rtgiIntensity)" );
		CvarSliderF( "Light Scale", r_rtgiLightScale, 0.0f, 8.0f, "%.2f", "Brightness of the diffuse lighting evaluated at a bounce "
			"hit. Raise for stronger colour bleed, lower if bounced light overpowers the direct lighting. (r_rtgiLightScale)" );
		CvarSliderF( "Emissive", r_rtgiEmissive, 0.0f, 16.0f, "%.2f", "How brightly a self-illuminated surface (neon, light tile) "
			"contributes when a bounce ray hits it, so glowing sources bleed their colour into the room. 0 = emitters bounce "
			"nothing. (r_rtgiEmissive)" );
		CvarSliderF( "Ray Length", r_rtgiRadius, 8.0f, 4096.0f, "%.0f units", "How far a bounce ray looks for the surface whose "
			"light it gathers. Longer = broader colour bleed and more distant bounce; shorter concentrates GI nearby and is "
			"cheaper on traversal. Default 720. (r_rtgiRadius)" );
		CvarSliderF( "Fog Awareness", r_rtgiFogAttenuate, 0.0f, 4.0f, "%.2f", "Fade the added bounce light out through the view's "
			"fog so GI stops washing out distance fog. 0 = off (old look), 1 = physical, >1 overdrives the falloff. Only in "
			"views with a fog light. Default 3.0. (r_rtgiFogAttenuate)" );
		CvarCheckbox( "GI Shadows", r_rtgiShadow, "Trace one shadow ray at each bounce hit toward the brightest reaching light, so "
			"a light behind a thin wall stops leaking indirect light through it. One extra ray per gather ray. 0 = unshadowed "
			"bounce (a little cheaper, lights leak through walls). (r_rtgiShadow)" );
	}

	// --- Irradiance cache: multi-bounce (docs/rtx-irradiance-cache.md) ---
	ImGui::SeparatorText( "Irradiance Cache (multi-bounce)" );
	{
		CvarCheckbox( "Irradiance Cache", r_rtgiCache, "A world-space, camera-adaptive hashed cache of the INDIRECT light every "
			"surface receives, learned from the GI bounce at each pixel and read back where a bounce ray lands - so a bounce hit "
			"is lit by the light bouncing onto IT too. Over a few frames the one bounce becomes all orders of diffuse bounce "
			"(corridor side walls, undersides and ceilings lit from below stop being black) and the bounce the denoiser sees is "
			"smoother. Off = the one-bounce GI as before. (r_rtgiCache)" );
		ImGui::BeginDisabled( !r_rtgiCache.GetBool() );
		CvarCheckbox( "Path Tail (serve the bounce)", r_rtgiCacheTail, "Tail mode, only while DI lights the scene (dude_rtx 2): a "
			"converged cell stores the surface's whole OUTGOING light and SERVES the bounce hit - the hit reads the cached value "
			"(plus its own emissive) and skips its light loop and shadow ray, so the gather is cheaper and the bounce the denoiser "
			"sees is cell + frame averaged (far less 1-spp noise) instead of a single direct shade. Off = indirect-only: the cell "
			"holds only the second-and-later-bounce light added on top of the hit's full live shade (the original behaviour). A "
			"change clears and re-learns the cache. (r_rtgiCacheTail)" );
		CvarSliderF( "Serve Error", r_rtgiCacheServeError, 0.0f, 1.0f, "%.2f", "Tail mode: how a cell decides it is converged "
			"enough to serve a bounce hit. > 0 = by variance: serve only once the mean's relative standard error is within this "
			"fraction, so a cell that has aged in but is still noisy keeps shading live until it settles - softens the pop as a "
			"cell converges, for a few more live-shaded hits (~0.2 ms gather). 0.33 (default) is the tuned sweet spot; lower = "
			"stricter / cleaner, higher = looser. 0 = by age instead (serve once the cell has >= 4 frames; the original C6 A/B). "
			"The Servable cache view shows which cells pass. (r_rtgiCacheServeError)" );
		CvarSliderF( "Cache Weight", r_rtgiCacheScale, 0.0f, 2.0f, "%.2f", "Weight of the cached (second and later bounce) light "
			"added at a bounce hit. 1 = physical; lower for a subtler multi-bounce fill. (r_rtgiCacheScale)" );
		CvarSliderF( "Cache History (ms)", r_rtgiCacheHistoryMs, 0.0f, 2000.0f, "%.0f ms", "How long a cell remembers, in "
			"milliseconds (the same TIME at 60 or 144 Hz): how long a switched or swinging light's OLD indirect contribution "
			"can linger against how smooth the cached term is. Default 500. 0 = use the frame count below. (r_rtgiCacheHistoryMs)" );
		CvarSliderI( "Cache History (frames)", r_rtgiCacheHistory, 1, 255, "Frames of memory per cell (Nmax) when the "
			"millisecond knob is 0. Default 32. (r_rtgiCacheHistory)" );
		CvarSliderF( "Cell Angle", r_rtgiCacheCellAngle, 0.001f, 0.02f, "%.4f", "Cache cell width per unit of eye distance "
			"(tan of half the cell's screen angle; 0.004 is ~10 pixels at 1440p). Smaller = sharper indirect light, fewer "
			"thin-wall leaks, slower convergence; larger = smoother, blurrier, leaks sooner. (r_rtgiCacheCellAngle)" );
		{
			int slotsIdx = idMath::ClampInt( 0, 3, r_rtgiCacheSlotsLog2.GetInteger() - 19 );
			ImGui::SetNextItemWidth( 240.0f );
			if ( ImGui::Combo( "Cells##rtgiCacheSlots", &slotsIdx, "512K cells (2 x 10 MB)\0" "1M cells (2 x 20 MB)\0" "2M cells (2 x 40 MB)\0" "4M cells (2 x 80 MB)\0" ) ) {
				r_rtgiCacheSlotsLog2.SetInteger( 19 + slotsIdx );
			}
			AddTooltip( "Cache size. More cells = fewer collisions and dropped updates in large open areas; a change re-allocates "
				"and clears the cache. Default 1M. (r_rtgiCacheSlotsLog2)" );
		}
		CvarCheckbox( "Gradient Reset", r_rtgiCacheReset, "Let the DI gradient restart a cell's history where the direct light "
			"changed by more than 35% (a switched or swinging light), so the cached indirect term follows at the DI's own speed. "
			"Diagnostic A/B: if the activity view keeps flashing red in a static scene, compare off and on. (r_rtgiCacheReset)" );
		{
			int dbg = idMath::ClampInt( 0, 6, r_rtgiCacheDebug.GetInteger() );
			ImGui::SetNextItemWidth( 240.0f );
			if ( ImGui::Combo( "Cache View##rtgiCacheDebug", &dbg, "Off\0Cell colouring\0Cached value\0Cell age\0Cell history (N)\0Cell activity (reset / fresh)\0Servable (tail mode)\0" ) ) {
				r_rtgiCacheDebug.SetInteger( dbg );
			}
			AddTooltip( "Overlay the cache over the view: 'Cell colouring' tints every cell (dim = not in the cache yet) and "
				"proves the keying - cells must stay the same size on screen as you move and split at normal-bin faces; "
				"'Cached value' shows the learned indirect light per cell; 'Cell age' frames since a cell's last update (white "
				"= evictable, dark red = absent); 'Cell history' N over Nmax (black = absent, 15% grey = just created, white = "
				"converged); 'Cell activity' red = reset this frame, green = created / seeded this frame, grey = steady - a "
				"static scene must settle to all grey; 'Servable' (tail mode) white where a cell is converged + fresh enough "
				"to serve the bounce hit, so a static view goes white in ~1 s. (r_rtgiCacheDebug)" );
		}
		ImGui::EndDisabled();	// r_rtgiCache
	}

	ImGui::EndDisabled();	// r_rtgi
	ImGui::EndDisabled();	// rtCapable
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_ReStirDI()
{
	// Live tuning + debug view for ReSTIR direct lighting (docs/rtx-restir-di.md, H7 'Path Tracing'). A
	// fresh 1-spp direct-lighting gather of the scene lights, denoised by the shared NRD REBLUR_DIFFUSE slot.
	// Bring-up feature, enabled directly here (not preset-wired yet). These knobs tune the denoiser's spatial
	// cleanup of the residual 1-spp noise NRD leaves on fine detail (edge-stopped) + moving parts (thin history).
	const bool rtCapable = R_SupportsRayTracing();
	if ( BeginSettingsGroup( "ReSTIR Direct Lighting" ) ) {

	if ( !rtCapable ) {
		ImGui::TextDisabled( "Requires the Vulkan backend + ray-query (RT) hardware." );
	}
	ImGui::BeginDisabled( !rtCapable );

	CvarCheckbox( "Enable ReSTIR DI", r_rtReStirDI, "Reservoir direct lighting: ONE shaded sample + ONE shadow ray "
		"per pixel for the scene's lights, NRD-denoised, instead of the ray-per-light loop - the per-pixel cost "
		"stops scaling with light count. Bring-up feature (not wired into the quality presets yet). Needs the "
		"velocity prepass (FSR2 or Motion Vectors) for the denoiser, and it borrows the shared GI NRD slot, so turn "
		"RTGI and RTAO OFF for it to denoise. Nothing consumes the result in the lit scene yet - use the debug view "
		"below. (r_rtReStirDI)" );

	ImGui::BeginDisabled( !r_rtReStirDI.GetBool() );

	// --- Stage D: DI lights the scene ---
	ImGui::SeparatorText( "Scene Lighting" );
	CvarCheckbox( "Light the Scene with DI", r_rtReStirDIApply, "The denoised DI, shaded with the G-buffer material "
		"(albedo + GGX roughness/metalness + spec map), becomes the direct lighting of every opaque world surface, "
		"and the forward per-light interaction draws + their shadow work are skipped for every light DI covers. "
		"The view weapon, translucents, blood films, fog/blend + ambient lights and sub-views stay forward. "
		"Off = DI is debug-only and the forward path is untouched. A/B: same look, cheaper in light-dense rooms. "
		"The debug view below overrides this. (r_rtReStirDIApply)" );

	// --- Debug view (the DI buffer blitted over the scene), the RTGI dropdown ---
	ImGui::SeparatorText( "Debug View" );
	{
		int dbg = idMath::ClampInt( 0, 2, r_rtReStirDIDebug.GetInteger() );
		ImGui::SetNextItemWidth( 240.0f );
		if ( ImGui::Combo( "Buffer##restirDiDebug", &dbg, "Off (lights the scene / hidden)\0Raw 1-spp gather\0NRD-denoised\0" ) ) {
			r_rtReStirDIDebug.SetInteger( dbg );
		}
		AddTooltip( "Blit the DI buffer over the finished frame ('Light the Scene with DI' is suppressed while this "
			"is on). 'Raw 1-spp gather' = the noisy input before denoising - with Checkerboard Gather on, the traced "
			"half is visibly packed into the LEFT half of the buffer (that is how to confirm the checkerboard is "
			"live); 'NRD-denoised' = the denoised result (falls back to raw if the denoise did not run). The view "
			"weapon reads as no-DI (masked). YCoCg-decoded and LINEAR - the HDR pipeline exposes it, so read "
			"structure and noise, not absolute brightness. (r_rtReStirDIDebug)" );
	}

	// --- Sampling ---
	ImGui::SeparatorText( "Sampling" );
	CvarSliderI( "Candidates", r_rtReStirDICandidates, 0, 128, "Light candidates per pixel before the weighted-reservoir "
		"pick. 0 = every light in the buffer is weighed (exhaustive: no candidate noise, a far dominant light stays at full "
		"strength). N > 0 = N uniform random draws: cheaper in light-dense views, but a rarely-drawn light arrives as "
		"sparse spikes the denoiser clips and reads dimmer. (r_rtReStirDICandidates)" );

	// --- Light grid (docs/rtx-light-grid.md): a world-space ReGIR candidate grid - the gather (and the GI bounce) pick
	// from each point's cell's short list of importance-sampled lights instead of weighing EVERY light. A perf win that
	// grows with light count; on by default at the Path Tracing tier. ---
	CvarCheckbox( "Light Grid", r_rtLightGrid, "Replace the gather's per-pixel scan over every light with a world-space "
		"grid of camera-centred cells, each filled once per frame with a few importance-sampled light candidates; the "
		"gather and the GI bounce then pick from their cell instead. A performance win that grows with the light count "
		"(measured -29% of the gather at ~40 lights), unbiased - same brightness as exhaustive, the denoiser absorbs the "
		"tiny extra candidate noise. On by default at the Path Tracing tier. (r_rtLightGrid)" );
	if ( r_rtLightGrid.GetBool() ) {
		ImGui::BeginDisabled( !rtCapable );
		CvarSliderI( "  Candidates / Cell", r_rtLightGridSlots, 1, 16, "Lights stored per cell. More = less candidate "
			"noise, closer to exhaustive, but a bigger grid buffer and a dearer fill. Default 16. A change re-allocates. (r_rtLightGridSlots)" );
		CvarSliderF( "  Cell Size", r_rtLightGridCellBase, 8.0f, 128.0f, "%.0f u", "Cascade-0 cell size in world units "
			"(each further cascade doubles it). Smaller = finer candidate sets near you but the grid re-centres in smaller "
			"steps as you move. Default 16. (r_rtLightGridCellBase)" );
		CvarSliderI( "  Resolution", r_rtLightGridRes, 8, 64, "Cells per axis in each cascade (the grid is res^3 x cascades). "
			"Higher = finer candidate sets but more cells to fill and store. Default 32. A change re-allocates. (r_rtLightGridRes)" );
		CvarSliderI( "  Cascades", r_rtLightGridCascades, 1, 4, "Camera-centred cascades (each twice the reach of the last). "
			"At the default 16-unit cell size, 4 cascades reach ~+-256/512/1024/2048 u. Default 4. A change re-allocates. (r_rtLightGridCascades)" );
		{
			int gdbg = idMath::ClampInt( 0, 4, r_rtLightGridDebug.GetInteger() );
			ImGui::SetNextItemWidth( 240.0f );
			if ( ImGui::Combo( "  Grid View##rtLightGridDebug", &gdbg, "Off\0Cascade\0Candidate count\0Dominant light\0Importance\0" ) ) {
				r_rtLightGridDebug.SetInteger( gdbg );
			}
			AddTooltip( "Debug overlay: 'Cascade' tints each cascade (red near, then green/blue/yellow out); 'Candidate "
				"count' per cell (black = empty / fell back); 'Dominant light' a stable tint of the cell's strongest light "
				"(flicker where two lights are near-equal is just the single-winner view, harmless); 'Importance' the cell's "
				"summed importance. (r_rtLightGridDebug)" );
		}
		ImGui::EndDisabled();
	}

	// --- Resolution (mirrors RT Global Illumination -> Resolution) ---
	ImGui::SeparatorText( "Resolution" );
	{
		const float diResStops[]  = { 0.25f, 0.5f, 0.667f, 0.75f, 1.0f };
		const char *diResLabels[] = { "Quarter (1/4)", "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Full" };
		CvarStopsSliderF( "Gather Resolution", r_rtReStirDIResScale, diResStops, diResLabels, IM_ARRAYSIZE( diResStops ),
			"Fraction of the screen the DI gather (and its NRD denoise) runs at, upscaled back to full res. Direct "
			"lighting is higher-frequency than GI, so dropping below full softens shadow/contact detail more than it "
			"does for RTGI - but it is the single biggest cost lever (~1/scale^2). Default Full. (r_rtReStirDIResScale)" );
	}
	CvarCheckbox( "Checkerboard Gather", r_rtReStirDICheckerboard, "Trace a horizontal checkerboard - half the pixels "
		"(packed into the buffer's left half, the traced parity alternating each frame) - and let NRD reconstruct the "
		"rest from a full-res guide pass + motion vectors, instead of shading every pixel. ~half the gather + "
		"visibility rays, keeping edges sharp (unlike a reduced-res scale). Composes with Gather Resolution (traces "
		"half of those pixels); sharpest at Full. Needs the velocity prepass. (r_rtReStirDICheckerboard)" );
	CvarCheckbox( "Bilateral Upscale", r_rtReStirDIBilateralUpscale, "Upscale a reduced-res gather with a depth+normal-"
		"aware filter instead of plain bilinear, so DI stops bleeding across silhouettes and creases - sharper at edges "
		"for a few extra taps. Inert at Full resolution. Off = the old bilinear look. (r_rtReStirDIBilateralUpscale)" );

	// --- Noise suppression (the full NRD tuning surface) ---
	// DI is ReLAX-only since R6. The knobs below map 1:1 to the NRD ReLAX settings the DI denoise sets
	// (VulkanBackend::RtaoDenoise); they clean the sharp 1-spp signal that RTGI's low-frequency bounce does not need.
	// 0 generally means "keep NRD's own default". History Confidence is the R1 temporal gradient (not the old luminance
	// pass, removed at R6); it drives NRD's IN_DIFF_CONFIDENCE so history is dropped only where the lighting changed.
	ImGui::SeparatorText( "Noise Suppression" );
	ImGui::TextDisabled( "NRD temporal history" );
	CvarSliderI( "History Length", r_rtReStirDIHistory, 2, 63, "Max frames NRD accumulates for the DI radiance "
		"(ReLAX diffuseMaxAccumulatedFrameNum). The 1-spp gather needs accumulation to look smooth: too short and single "
		"pixels 'fry', too long and a switched-off/occluded light lingers. ~6-12 is the balance; NRD default 30. (r_rtReStirDIHistory)" );
	CvarSliderF( "History (ms)", r_rtReStirDIHistoryMs, 0.0f, 2000.0f, "%.0f ms", "History window in MILLISECONDS instead "
		"of frames (converted per frame from a 0.2 s low-passed FPS), so the window is the same wall-clock time at 60 or "
		"144 Hz. 0 = use History Length in frames. (r_rtReStirDIHistoryMs)" );
	CvarSliderI( "Fast History", r_rtReStirDIRelaxFast, 1, 63, "ReLAX diffuseMaxFastAccumulatedFrameNum - the short 'fast' "
		"history the long one is clamped to; the main built-in anti-lag. Remix direct light ships 1 ('to reduce trail'); NRD "
		"default 6. (r_rtReStirDIRelaxFast)" );
	CvarSliderF( "Fast Clamp", r_rtReStirDIFastClamp, 1.0f, 3.0f, "%.1f", "ReLAX fastHistoryClampingSigmaScale [1;3] - how "
		"tightly the slow history is clamped to the fast one. Lower = less lag, more flicker. NRD default 2.0; the NRD sample "
		"uses 1.5. (r_rtReStirDIFastClamp)" );
	// Interlock: Gradient Confidence (below) REPLACES NRD's built-in anti-lag - running both double-rejects
	// history on every lighting change. So anti-lag is forced inert while the gradient is on (RhiWorld.cpp's
	// da.antilagOff); grey it out here to match the engine.
	ImGui::BeginDisabled( r_rtReStirDIGradient.GetBool() );
	CvarCheckbox( "Anti-lag", r_rtReStirDIAntilag, "NRD's built-in luminance anti-lag. On = NRD defaults; off = inert (the "
		"baseline NRD recommends for an initial integration and for the gradient confidence). Forced OFF while "
		"Gradient Confidence is on - the gradient is its targeted replacement. (r_rtReStirDIAntilag)" );
	if ( r_rtReStirDIGradient.GetBool() ) {
		ImGui::TextDisabled( "  (off - Gradient Confidence replaces it)" );
	}
	ImGui::EndDisabled();
	CvarSliderF( "Disocclusion", r_rtReStirDIDisocclusion, 0.005f, 0.05f, "%.3f", "NRD disocclusionThreshold - relative "
		"(slope-scaled) depth difference above which reprojected history is rejected as a different surface. Looser = fewer "
		"FALSE resets on movers / the weapon, at some bleed across real depth edges. NRD range 0.01..0.02. (r_rtReStirDIDisocclusion)" );
	ImGui::Spacing();
	ImGui::TextDisabled( "NRD spatial (ReLAX A-trous)" );
	CvarSliderI( "History Fix", r_rtReStirDIHistoryFix, 0, 8, "ReLAX historyFixFrameNum - frames of strong SPATIAL "
		"reconstruction where temporal history is thin (a fresh disocclusion or a moving limb): the lever for noise on "
		"MOVING parts. Clamped below the fast-history window. 0 = NRD default (3). (r_rtReStirDIHistoryFix)" );
	CvarSliderF( "Prepass Blur", r_rtReStirDIRelaxPrepass, 0.0f, 100.0f, "%.0f px", "ReLAX diffusePrepassBlurRadius - a "
		"pre-accumulation spatial blur for the 1-spp input. Remix direct light ships 0; NRD default 30. A few px can help "
		"the no-spatial-reuse gather, but too large blobs sparse bright samples. (r_rtReStirDIRelaxPrepass)" );
	CvarSliderF( "Phi Luminance", r_rtReStirDIRelaxPhi, 0.1f, 8.0f, "%.1f", "ReLAX diffusePhiLuminance - the A-trous "
		"luminance edge-stopping sensitivity. Lower = sharper shadow edges, more residual noise. RTXDI sample 1.0, NRD "
		"default 2.0. (r_rtReStirDIRelaxPhi)" );
	CvarSliderI( "A-trous Iterations", r_rtReStirDIRelaxAtrous, 2, 8, "ReLAX atrousIterationNum [2;8] - the spatial filter "
		"passes. More = cleaner but softer + costlier (this is the biggest ReLAX cost lever). NRD default 5; we ship 3. "
		"(r_rtReStirDIRelaxAtrous)" );
	CvarSliderI( "Variance History", r_rtReStirDIRelaxVarianceHist, 0, 10, "ReLAX spatialVarianceEstimationHistoryThreshold "
		"- below this history length the variance is estimated spatially. RTXDI sample 1, NRD default 3. (r_rtReStirDIRelaxVarianceHist)" );
	CvarSliderF( "Confidence Relax", r_rtReStirDIRelaxConfRelax, 0.0f, 2.0f, "%.2f", "ReLAX confidenceDrivenRelaxationMultiplier "
		"- where the gradient confidence is low, relax the A-trous luminance / normal edge-stopping. Remix direct: 0.7. 0 = off. "
		"(r_rtReStirDIRelaxConfRelax)" );
	CvarSliderF( "Edge-Stop Relax", r_rtReStirDIEdgeStop, 0.0f, 16.0f, "%.1f", "Scales ReLAX's normal edge-stopping "
		"(lobeAngleFraction). >1 lets the spatial passes blur ACROSS depth/normal edges - the only lever that reaches "
		"sub-pixel GRATE noise the others can't, at the cost of softening that fine detail. 1 / 0 = no scaling. (r_rtReStirDIEdgeStop)" );
	CvarCheckbox( "Anti-Firefly", r_rtReStirDIRelaxFirefly, "ReLAX enableAntiFirefly - clamps lone bright fireflies (+7-10% "
		"ReLAX cost). NRD default off; Remix / the RTXDI sample ship it on. (r_rtReStirDIRelaxFirefly)" );
	ImGui::Spacing();
	ImGui::TextDisabled( "History Confidence (R1 temporal gradient)" );
	CvarCheckbox( "Gradient Confidence", r_rtReStirDIGradient, "Re-shade last frame's chosen light at the reprojected "
		"surface under this frame's lights, A-trous filter the change, and feed NRD IN_DIFF_CONFIDENCE so history is dropped "
		"ONLY where the lighting really changed (RTXDI / Q2RTX recipe). The DI anti-lag; default on. (r_rtReStirDIGradient)" );
	ImGui::BeginDisabled( !r_rtReStirDIGradient.GetBool() );
	CvarSliderF( "  Power", r_rtReStirDIGradientPower, 1.0f, 32.0f, "%.0f", "Sharpening power of (1 - relative change) -> "
		"confidence. Higher = a smaller change already rejects history. RTXDI / Remix 8. (r_rtReStirDIGradientPower)" );
	CvarSliderF( "  Rise Smoothing", r_rtReStirDIGradientHistory, 0.0f, 32.0f, "%.0f", "Temporal smoothing (frames) applied "
		"ONLY while confidence RISES after a change (drops apply at once). Our gather is fresh every frame, so 2 just bridges "
		"gradient flicker; 0 = none. (r_rtReStirDIGradientHistory)" );
	CvarSliderF( "  Floor", r_rtReStirDIGradientFloor, 0.0f, 1.0f, "%.2f", "Minimum confidence (ReLAX applies it directly). "
		"Remix 0.1. (r_rtReStirDIGradientFloor)" );
	CvarSliderF( "  Darkness Bias", r_rtReStirDIGradientBias, 0.0f, 1.0f, "%.3f", "Darkness bias added to the change's "
		"denominator (RTXDI darknessBias) so dark-on-dark flicker does not read as a 100% change. (r_rtReStirDIGradientBias)" );
	CvarSliderF( "  Deadzone", r_rtReStirDIGradientDeadzone, 0.0f, 0.9f, "%.2f", "Relative per-light change below which "
		"nothing counts as a lighting change. 0.2 ignores Doom's authored 5-20% flicker; a light switching off is still a "
		"100% change. (r_rtReStirDIGradientDeadzone)" );
	CvarSliderF( "  Static Threshold", r_rtReStirDIGradientStatic, 0.0f, 16.0f, "%.1f", "Screen-space motion (full-res px) "
		"below which a surface point is treated as static and re-shaded at its exact stored previous position, so a static "
		"world under a static light compares bit-identical (no jitter frying). 0 = always current surface. (r_rtReStirDIGradientStatic)" );
	CvarSliderI( "  Stratum", r_rtReStirDIGradientStratum, 2, 6, "One re-shade + one ray per NxN block of the gather res. "
		"3 = RTXDI (1/9 of pixels); 5 = NRD's 1/5 res (~1/3 the cost). The A-trous reach scales with it. (r_rtReStirDIGradientStratum)" );
	CvarSliderI( "  Filter Passes", r_rtReStirDIGradientFilterPasses, 0, 4, "A-trous passes over the gradient (strides "
		"1/2/4/8). 4 = RTXDI (wide, smooth reset); 2 hugs a change tighter (a walking NPC's shadow edge); 0 = unfiltered. "
		"(r_rtReStirDIGradientFilterPasses)" );
	ImGui::EndDisabled();	// r_rtReStirDIGradient

	ImGui::EndDisabled();	// r_rtReStirDI
	ImGui::EndDisabled();	// rtCapable
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_ContactShadows()
{
	// Screen-space contact shadows (docs/contact-shadows.md): a short per-light depth march that
	// grounds the small geometry shadow maps miss - dropped pickups, thin props, weapon detail,
	// feet. Raster tiers (opengl3 + Vulkan); at Path Tracing r_rtAllLights covers it, so leave
	// this off there. Not preset-wired - a look change, tuned live here.
	const bool backendOk = R_BackendSupportsEnhancements();
	if ( BeginSettingsGroup( "Contact Shadows", &r_contactShadows,
			"r_contactShadows: march the depth buffer from each lit pixel toward the light and darken where a "
			"nearer occluder is found. Grounds small props/pickups/weapon detail/feet that shadow maps are too "
			"coarse to catch. opengl3/Vulkan only. Automatically disabled while RT all-lights is on "
			"(Path Tracing's per-light RT rays supersede it)." ) ) {

		const bool rtAll = r_rtAllLights.GetBool();
		if ( !backendOk ) {
			ImGui::TextDisabled( "Needs the opengl3 or Vulkan backend." );
		} else if ( rtAll ) {
			ImGui::TextDisabled( "Superseded by RT all-lights (Path Tracing) - inactive while that is on." );
		}
		ImGui::BeginDisabled( !backendOk || rtAll );

		CvarSliderF( "Strength", r_contactShadowStrength, 0.0f, 1.0f, "%.2f", "r_contactShadowStrength: how much a found occluder dims the light (0 = none, 1 = full black "
			"at the contact). Faded by march distance so a nearer occluder darkens more. Default 0.8." );

		CvarSliderF( "March Length", r_contactShadowLength, 1.0f, 128.0f, "%.0f", "r_contactShadowLength: how far toward the light the depth march reaches, world units. Short "
			"= tight contact grounding; long = broader but coarser and dearer. Clamped to the distance to the "
			"light. Default 12." );

		CvarSliderF( "Occluder Thickness", r_contactShadowThickness, 0.5f, 128.0f, "%.0f", "r_contactShadowThickness: a depth sample nearer than the ray by more than this is treated as "
			"background, not an occluder. Too low = shadows drop out behind thin geometry; too high = haloing / "
			"over-occlusion. Default 8." );

		CvarSliderI( "March Steps", r_contactShadowSteps, 2, 32, "r_contactShadowSteps: depth-march samples per lit pixel. More = smoother/further-reaching, "
			"dearer. Runs per light, so keep it low (watch r_gl3GpuTime / r_vkGpuTime). Default 8." );

		ImGui::EndDisabled();	// backendOk
		EndSettingsGroup();
	}
}

static void DrawDbgGroup_DepthOfField()
{
	// Weapon-reload depth-of-field (r_dof): as a weapon reloads, the world beyond it
	// blurs so the view pulls focus onto the gun. Eases in on reload start, holds across
	// multi-shell reloads, eases out at the end. RHI backends only. The on/off toggle now
	// lives in Graphics -> Post-Processing; these are the live tuning sliders.
	if ( BeginSettingsGroup( "Depth of Field (weapon reload)" ) ) {

		const bool backendOk = R_BackendSupportsEnhancements();
		if ( !backendOk ) {
			ImGui::TextDisabled( "Needs the opengl3 or Vulkan backend." );
		}
		if ( !r_dof.GetBool() ) {
			ImGui::TextDisabled( "Enable in Graphics -> Post-Processing." );
		}
		ImGui::BeginDisabled( !backendOk || !r_dof.GetBool() );

		float radius = r_dofBlurRadius.GetFloat();
		if ( ImGui::SliderFloat( "Blur Radius", &radius, 0.0f, 64.0f, "%.0f" ) ) {
			r_dofBlurRadius.SetFloat( radius );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofradius" ) ) { r_dofBlurRadius.Reset(); }
		AddTooltip( "r_dofBlurRadius: maximum world blur radius in pixels at full focus. "
			"Higher = stronger, dreamier blur; ~6 is a subtle default." );

		float focusStart = r_dofFocusStart.GetFloat();
		if ( ImGui::SliderFloat( "Focus Start", &focusStart, 0.0f, 1.0f, "%.2f" ) ) {
			r_dofFocusStart.SetFloat( focusStart );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofstart" ) ) { r_dofFocusStart.Reset(); }
		AddTooltip( "r_dofFocusStart: scene depth (0..1, hyperbolic) where the blur starts ramping. "
			"The weapon sits below this and stays sharp - raise it if the gun itself softens." );

		float focusEnd = r_dofFocusEnd.GetFloat();
		if ( ImGui::SliderFloat( "Focus End", &focusEnd, 0.0f, 1.0f, "%.2f" ) ) {
			r_dofFocusEnd.SetFloat( focusEnd );
		}
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##dofend" ) ) { r_dofFocusEnd.Reset(); }
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

static void DrawDbgGroup_EyeAdaptation()
{
	// HDR eye adaptation (Phase B1) fine-tuning. The on/off toggle lives in
	// Graphics -> Post-Processing (needs r_hdr + a tonemap curve); these are the knobs.
	if ( BeginSettingsGroup( "Eye Adaptation (auto-exposure)" ) ) {

		const bool backendOk = R_BackendSupportsEnhancements();
		if ( !backendOk ) {
			ImGui::TextDisabled( "Needs the opengl3 or Vulkan backend." );
		}
		const bool eye = cvarSystem->GetCVarBool( "r_hdrEyeAdaptation" );
		if ( !eye ) {
			ImGui::TextDisabled( "Enable Eye Adaptation in Graphics -> Post-Processing." );
		}
		ImGui::BeginDisabled( !backendOk || !eye );

		float aspeed = cvarSystem->GetCVarFloat( "r_hdrAdaptSpeed" );
		if ( ImGui::SliderFloat( "Adapt Speed", &aspeed, 0.1f, 10.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptSpeed", aspeed );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaSpeed" ) ) { ResetNamedCvar( "r_hdrAdaptSpeed" ); }
		AddTooltip( "r_hdrAdaptSpeed: adaptation rate; higher = faster (time constant ~1/speed seconds)." );

		float ebri = cvarSystem->GetCVarFloat( "r_hdrAdaptBrighten" );
		if ( ImGui::SliderFloat( "Brighten in Dark", &ebri, 0.0f, 4.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptBrighten", ebri );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaBri" ) ) { ResetNamedCvar( "r_hdrAdaptBrighten" ); }
		AddTooltip( "r_hdrAdaptBrighten: exposure ADDED to the base Exposure as the scene darkens (dark\n"
			"areas open up). Eases smoothly to this cap, never pins." );

		float edar = cvarSystem->GetCVarFloat( "r_hdrAdaptDarken" );
		if ( ImGui::SliderFloat( "Darken in Light", &edar, 0.0f, 4.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptDarken", edar );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaDar" ) ) { ResetNamedCvar( "r_hdrAdaptDarken" ); }
		AddTooltip( "r_hdrAdaptDarken: exposure REMOVED from the base Exposure as the scene brightens\n"
			"(bright scenes dim). Keep small for a mild pull-down." );

		float ekey = cvarSystem->GetCVarFloat( "r_hdrAdaptKey" );
		if ( ImGui::SliderFloat( "Reference Luminance", &ekey, 0.005f, 0.08f, "%.3f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptKey", ekey );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaKey" ) ) { ResetNamedCvar( "r_hdrAdaptKey" ); }
		AddTooltip( "r_hdrAdaptKey: the scene luminance treated as neutral (mapped to the base Exposure).\n"
			"Sit it in the middle of the scene's range so both directions engage; too high and every\n"
			"scene reads as 'dark' so it only ever brightens." );

		float ecen = cvarSystem->GetCVarFloat( "r_hdrAdaptCenter" );
		if ( ImGui::SliderFloat( "Metering Region", &ecen, 0.15f, 1.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptCenter", ecen );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaCen" ) ) { ResetNamedCvar( "r_hdrAdaptCenter" ); }
		AddTooltip( "r_hdrAdaptCenter: central fraction of the view measured. 1 = whole screen (the dark\n"
			"periphery washes out what you look at); lower = center-weighted, so aiming at a light or a\n"
			"dark corner actually moves the exposure." );

		float egrain = cvarSystem->GetCVarFloat( "r_hdrAdaptGrain" );
		if ( ImGui::SliderFloat( "Low-light Grain Boost", &egrain, 0.0f, 5.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptGrain", egrain );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaGrain" ) ) { ResetNamedCvar( "r_hdrAdaptGrain" ); }
		AddTooltip( "r_hdrAdaptGrain: raise film grain as the dark-scene brightening ramps up (high-gain\n"
			"sensor/eye noise). 1 = grain ~doubles at full brightening; 0 = off. Needs Film Grain > 0." );

		float edesat = cvarSystem->GetCVarFloat( "r_hdrAdaptDesat" );
		if ( ImGui::SliderFloat( "Low-light Desaturation", &edesat, 0.0f, 1.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptDesat", edesat );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaDesat" ) ) { ResetNamedCvar( "r_hdrAdaptDesat" ); }
		AddTooltip( "r_hdrAdaptDesat: wash colours toward gray as the dark-scene brightening ramps up\n"
			"(scotopic vision — rods take over in the dark). 0.33 = colours drop to ~67% at full\n"
			"brightening; 0 = off." );

		float ewp = cvarSystem->GetCVarFloat( "r_hdrAdaptWhitePoint" );
		if ( ImGui::SliderFloat( "DUDE Adaptive White Point (exp.)", &ewp, 0.0f, 1.0f, "%.2f" ) ) {
			cvarSystem->SetCVarFloat( "r_hdrAdaptWhitePoint", ewp );
		}
		ImGui::SameLine(); if ( ImGui::SmallButton( "reset##eaWP" ) ) { ResetNamedCvar( "r_hdrAdaptWhitePoint" ); }
		AddTooltip( "r_hdrAdaptWhitePoint (EXPERIMENTAL, DUDE tonemap only): the DUDE shoulder's white point\n"
			"tracks the scene's smoothed brightest metered spot, so a bright lamp/fire core keeps its\n"
			"internal gradient instead of flattening to a white disc when auto-exposure ramps up in the\n"
			"dark. 0 = off (static shoulder); 1 = full tracking. Extend-only — never rolls off harder\n"
			"than the static curve, so turning it up can't crush highlights vs the default." );

		ImGui::Spacing();
		bool dbg = cvarSystem->GetCVarBool( "r_hdrEyeAdaptDebug" );
		if ( ImGui::Checkbox( "Debug boxes (exposure / luminance)", &dbg ) ) {
			cvarSystem->SetCVarBool( "r_hdrEyeAdaptDebug", dbg );
		}
		AddTooltip( "r_hdrEyeAdaptDebug: overlay two boxes low on screen — left = adapted exposure,\n"
			"right = measured scene luminance — to see the metering while you tune." );

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

		CvarCheckbox( "GPU skinning", r_gpuSkinning, "r_gpuSkinning: skin animated meshes on the GPU. ON by default on Vulkan (and from the "
			"Medium preset up); Potato/Low keep the exact CPU skin as the faithful floor. Offloads the DRAW; the CPU "
			"position skin still runs (it is the monster hit surface). Prerequisite for ray-tracing animated geometry "
			"later. Next map load." );

		CvarCheckbox( "GPU skin seam weld", r_gpuSkinWeld, "r_gpuSkinWeld: weld GPU-derived seam normals for "
			"crack-free PN tessellation, DECOUPLED from lighting (default ON). The geometry normal is welded (seams "
			"do not tear under tessellation) while the shading rebuilds the UN-welded normal from the tangents -- so "
			"there is no hard-edge shading 'cut' either. OFF: bit-identical to the CPU skinner (faithful shading) but "
			"tessellated characters can reopen the seam slits. Needs GPU skinning + r_gpuSkinDerive. Live (refreshes "
			"NPCs on toggle)." );

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

static void DrawDbgGroup_FSR()
{
	// FSR temporal pipeline (docs/fsr-temporal-pipeline.md). Motion vectors + jitter are the
	// inputs FSR2 (R1) consumes; the debug views expose the velocity buffer. Vulkan only — the
	// velocity MRT needs a float-colour render target the GL 3.3 backend doesn't have. VK is
	// rhiBackend && !coreProfile (RenderSystem.h; see the running-backend combo above).
	const bool isVulkan = glConfig.rhiBackend && !glConfig.coreProfile;
	if ( BeginSettingsGroup( "FSR / Motion Vectors (Vulkan only)" ) ) {
	ImGui::TextDisabled( "Motion-vector infrastructure + debug views. The FSR2 switch is in Graphics -> Antialiasing." );
	ImGui::Spacing();

	if ( !isVulkan ) {
		ImGui::TextDisabled( "Requires the Vulkan backend (velocity needs a float-colour render target)." );
	}
	ImGui::BeginDisabled( !isVulkan );

	// The user-facing FSR2 switch (+ sharpness/reactive) lives in Graphics -> Antialiasing;
	// this group keeps the motion-vector infrastructure + debug views only.
	CvarCheckbox( "Motion Vectors (r_motionVectors)", r_motionVectors, "Per-object screen-space motion vectors written into the normal G-buffer's 3rd MRT "
		"(R1/A2). Forces the normal prepass on (an extra opaque pass). Feeds the temporal SSAO/SSR "
		"reprojection so moving objects stop dragging a ghost, and — eventually — FSR2." );

	ImGui::BeginDisabled( !r_motionVectors.GetBool() );

	int mvDbg = r_mvDebug.GetInteger();
	const char *mvDbgItems[] = { "off (feed consumers)", "direction (R=+x, G=+y)", "magnitude" };
	if ( ImGui::Combo( "Debug View (r_mvDebug)", &mvDbg, mvDbgItems, IM_ARRAYSIZE( mvDbgItems ) ) ) {
		r_mvDebug.SetInteger( mvDbg );
	}
	AddTooltip( "Visualize the velocity buffer over the scene. 1 = direction (static = flat grey, the "
		"tint flips with motion); 2 = magnitude (black = no motion). Strafe / walk to see the geometry "
		"light up by depth; a camera pan concentrates velocity at the screen edges. 0 to feed the consumers." );

	CvarSliderF( "Debug Gain (r_mvDebugScale)", r_mvDebugScale, 1.0f, 40.0f, "%.0f", "Amplifies the debug overlay only — per-frame screen velocities are tiny, so crank this "
		"up to see slow motion. Does not affect the velocity fed to the consumers." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##mvscale" ) ) { r_mvDebugScale.Reset(); }

	ImGui::EndDisabled();	// !r_motionVectors

	ImGui::EndDisabled();	// !isVulkan
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

	CvarCheckbox( "Shadow Mapping (master)", r_shadowMapping, "Master toggle. On = shadow maps (stencil fully off); Off = vanilla stencil." );

	// isolate a single light by index to study its shadow in isolation; -1 = all
	int single = r_singleLight.GetInteger();
	if ( ImGui::InputInt( "Isolate Light (r_singleLight, -1=all)", &single ) ) {
		if ( single < -1 ) single = -1;
		r_singleLight.SetInteger( single );
	}
	AddTooltip( "Render only this light index (everything else black), to study one light's "
		"shadow. -1 shows all lights. Use r_shadowMapDebug 2 to read light indices in the console." );

	CvarSliderI( "Debug Print (r_shadowMapDebug)", r_shadowMapDebug, 0, 2, "0 = off, 1 = per-view summary, 2 = per-light readout (technique, occluder "
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
		r_shadowMapBias.Reset();
	}

	float modelBias = r_shadowMapModelBias.GetFloat();
	if ( ImGui::SliderFloat( "Model Bias", &modelBias, 0.0f, 0.05f, "%.4f" ) ) {
		r_shadowMapModelBias.SetFloat( idMath::ClampFloat( 0.0f, 0.5f, modelBias ) );
	}
	AddTooltip( "Separate bias for model (non-world) receivers. Curved model geometry "
		"self-shadows more, so it usually needs a larger bias than world surfaces." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##modelbias" ) ) {
		r_shadowMapModelBias.Reset();
	}

	int cull = r_shadowMapCull.GetInteger();
	const char *cullItems[] = { "front faces", "back faces (second-depth)", "two-sided" };
	if ( ImGui::Combo( "Caster Faces", &cull, cullItems, IM_ARRAYSIZE( cullItems ) ) ) {
		r_shadowMapCull.SetInteger( cull );
	}
	AddTooltip( "Which faces of occluders are rendered into the map. Back/second-depth (1) "
		"usually reduces acne; front (0) or two-sided (2) can fix light-leaks or thin-wall "
		"artifacts. Try these if shadows are missing/leaking on certain geometry." );

	bool perf = CvarCheckbox( "Perforated Casters (grates/fences)", r_shadowMapPerforated, "Alpha-tested grates/fences/foliage cast punched-out shadows." );

	ImGui::BeginDisabled( !perf );
	CvarSliderF( "Perforated Shadow Strength", r_shadowMapPerforatedStrength, 0.0f, 1.0f, "%.2f", "How dark grate/fence shadows get. 1 = fully dark like solid geometry; "
		"lower lets some light through, so a fence dims a room instead of blacking it "
		"out — closer to how vanilla faked these with soft light textures. Only affects "
		"perforated casters; solid-geometry shadows stay fully dark." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##perfstrength" ) ) {
		r_shadowMapPerforatedStrength.Reset();
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText( "Adaptive resolution (radius-scaled)" );

	bool sizeScale = CvarCheckbox( "Scale Resolution With Light Size", r_shadowMapSizeScale, "Give bigger lights more shadow resolution and smaller lights less, so a "
		"shadow texel maps to roughly the same world distance for every light. Cuts jagged "
		"edges on large/far lights. Tiers step in powers of two around the base resolutions "
		"on the main Shadows page (-1x to +4x), clamped to each map's own range." );

	ImGui::BeginDisabled( !sizeScale );
	CvarSliderF( "Reference Radius", r_shadowMapSizeScaleRadius, 16.0f, 2048.0f, "%.0f", "The light radius that maps to the base resolution. Lights larger than this "
		"get more resolution; smaller ones get less. Lower it to push more lights into the "
		"higher-resolution tiers." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##refradius" ) ) {
		r_shadowMapSizeScaleRadius.Reset();
	}
	ImGui::EndDisabled();

	ImGui::SeparatorText( "Cube map (point / omni lights)" );

	// Base 2D/cube resolutions and the point-light budget are owned by the main Shadows
	// page (one canonical control each) — only the dev-only cube tuning lives here.
	ImGui::TextDisabled( "Map resolutions and the point-light budget are on the main Shadows page." );
	ImGui::Spacing();

	CvarSliderI( "Cube Edge Softness (PCF taps)", r_shadowMapCubePcf, 1, 16, "Point-light cube shadows soften their edge by averaging this many disc-offset "
		"depth taps. 1 = a single hardware tap (hardest, blockiest, cheapest); 4 matches the "
		"2D/spot path; 6 (default) is a soft, well-filtered edge; higher smooths stair-stepping "
		"at low resolution but costs more per lit point-light fragment. Free on VRAM (filtering "
		"only)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##cubepcf" ) ) {
		r_shadowMapCubePcf.Reset();
	}

	CvarCheckbox( "Cull Off-Screen Faces", r_shadowMapFaceCull, "Skip rasterizing occluders into cube faces whose 90-degree cone can't reach "
		"the view frustum — those faces are never sampled by anything on screen. Big win at high "
		"resolution when a light is partly behind you. r_shadowMapDebug 1 shows faces drawn/culled." );

	CvarSliderF( "Cube Range Scale", r_shadowMapPointRangeScale, 0.1f, 32.0f, "%.2fx", "Scales the cube shadow's far plane + depth normalizer. If distant/floor "
		"shadows under a light are missing, this controls how far the map reaches. 1.0 = the "
		"light's own radius." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##range" ) ) {
		r_shadowMapPointRangeScale.Reset();
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
		r_shadowMapStencilRadius.Reset();
	}

	ImGui::SeparatorText( "Static cache (skip regenerating unchanged lights)" );

	bool cache = CvarCheckbox( "Cache Static Point Lights", r_shadowMapCache, "Keep each point light's cube map across frames and only re-render it when the "
		"light or one of its shadow casters moves. In mostly-static scenes this is the single "
		"biggest shadow-map speedup. r_shadowMapDebug 1 shows cache hit/rendered and MB used." );

	ImGui::BeginDisabled( !cache );
	const int vram = glConfig.vidMemMB;					// 0 if the vendor query failed
	const int cacheCvar = r_shadowMapCacheMB.GetInteger();
	bool autoBudget = ( cacheCvar < 0 );
	const int manualSeed = ( vram > 0 ? vram / 2 : 1024 );	// starting value when switching auto -> manual
	const int sliderMax = ( vram > 0 ? vram : 16384 );

	if ( ImGui::Checkbox( "Auto Budget (free VRAM - reserve)", &autoBudget ) ) {
		r_shadowMapCacheMB.SetInteger( autoBudget ? -1 : ( cacheCvar < 0 ? manualSeed : cacheCvar ) );
	}
	AddTooltip( "Auto sizes the cache to the VRAM actually free right now (measured) minus a safety "
		"reserve for other growth - as large as safely possible, so static cubes rarely evict and "
		"re-render (the frame-time cost). Scales to the card. Uncheck to cap it manually." );

	ImGui::BeginDisabled( autoBudget );
	int budget = ( cacheCvar < 0 ) ? manualSeed : cacheCvar;
	if ( ImGui::SliderInt( "Cache Budget", &budget, 0, sliderMax, budget == 0 ? "unlimited" : "%d MB" ) ) {
		r_shadowMapCacheMB.SetInteger( budget );
	}
	AddTooltip( "VRAM the shadow cache may use. 0 = unlimited (cache every static light). Lights "
		"that don't fit fall back to per-frame regeneration, so lowering this never breaks shadows, "
		"it just caches fewer of them." );
	ImGui::EndDisabled();	// auto budget

	if ( vram > 0 ) {
		ImGui::TextDisabled( "Detected VRAM: %d MB   (effective budget: %s)",
			vram, autoBudget ? "adaptive (free VRAM - reserve; see the VRAM HUD, r_vkDebugHud)"
			                 : ( budget == 0 ? "unlimited" : va( "%d MB", budget ) ) );
	}

	int dynDrop = idMath::ClampInt( 0, 2, r_shadowMapSplitDynDrop.GetInteger() );
	if ( ImGui::Combo( "Movers' Layer Resolution", &dynDrop, "Same as static\0Half\0Quarter\0" ) ) {
		r_shadowMapSplitDynDrop.SetInteger( dynDrop );
	}
	AddTooltip( "r_shadowMapSplitDynDrop: when a monster/mover shares a cached static light, its "
		"shadow renders into a separate cube that regenerates every frame. Rendering that cube at "
		"a lower resolution cuts the recurring per-frame cost; the moving shadow gets proportionally "
		"softer (filter and bias scale to match) while the cached static shadows stay full-res. "
		"Default: Half." );

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

	CvarSliderF( "Intensity", r_emissiveLightScale, 0.0f, 4.0f, "%.2f", "Brightness of the fill light a glowing screen casts. 0 = off, 0.50 = default, higher = a strong glow." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emscale" ) ) { r_emissiveLightScale.Reset(); }

	CvarSliderF( "Reach (distance)", r_emissiveLightRadius, 0.25f, 16.0f, "%.2f", "How far the fill light reaches, as a multiple of the screen's own size. Small/medium screens "
		"scale linearly; large ones roll off toward the Max Reach cap below. Default 1.80." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emradius" ) ) { r_emissiveLightRadius.Reset(); }

	CvarSliderF( "Max Reach (big signs)", r_emissiveLightMaxReach, 32.0f, 512.0f, "%.0f", "Soft cap (world units) that reach saturates toward, so a big hanging sign doesn't cast across "
		"the whole room. Lower = big signs reined in harder; raise toward 512 for near-linear scaling. Default 120." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emmaxreach" ) ) { r_emissiveLightMaxReach.Reset(); }

	CvarSliderF( "Fade-off", r_emissiveLightFalloff, 0.0f, 1.0f, "%.2f", "How far the glow reaches and how gently it fades. 0 = a tight, bright pool with a sharp edge; "
		"1 = a soft glow that reaches further out. Default 0.5." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emfalloff" ) ) { r_emissiveLightFalloff.Reset(); }

	CvarSliderF( "Beam Spread", r_emissiveLightSpread, 0.5f, 3.5f, "%.2f", "Width of the fill cone. 0.5 = a tight beam, 1.60 = default, 3.5 = a wide near-hemisphere "
		"that wraps around the screen (but still clipped behind the mount)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emspread" ) ) { r_emissiveLightSpread.Reset(); }

	CvarSliderF( "Colour saturation", r_emissiveLightSaturation, 0.0f, 1.0f, "%.2f", "How much of the screen's own colour the bleed keeps. 0 = white, 0.75 = default tint, "
		"1 = full screen hue (a coloured spotlight)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##emsat" ) ) { r_emissiveLightSaturation.Reset(); }

	ImGui::Spacing();

	CvarCheckbox( "Specular Highlights", r_emissiveLightSpecular, "On: fill lights add a specular glint (more visible on your weapon). "
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

	CvarSliderF( "Reflection brightness", r_gl3ReflectionScale, 0.0f, 1.5f, "%.2f", "Brightness of the environment-map reflection on glass. 1.00 = the untouched cube; "
		"0.70 = default, dampened 30% so the sheen matches legacy against the brighter enhanced scene." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##reflscale" ) ) { r_gl3ReflectionScale.Reset(); }

	ImGui::Spacing();
	ImGui::TextDisabled( "Decal sheen: texture mod packs make blood and dirt decals shiny. No effect on the stock assets." );

	CvarSliderF( "Blood decal sheen", r_decalBloodSheen, 0.0f, 1.0f, "%.2f", "Specular highlight + cube reflection on blood splats, smears and pools. "
		"0.00 = matte, 1.00 = as authored by the mod pack; 0.35 = default." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##bloodsheen" ) ) { r_decalBloodSheen.Reset(); }

	CvarCheckbox( "Matte dirt decals", r_decalMatte, "Dirt, stain and drip decals get no specular highlight and no cube reflection. "
		"Off = as authored by the mod pack." );

	CvarSliderF( "Decals dull metal", r_decalMetalMask, 0.0f, 1.0f, "%.2f", "Dirt on metal is not metal: a decal removes the metalness of the surface under it by its "
		"coverage, so stains read matte against the clean plate around them. Stock and mod decals alike. "
		"0.00 = off, 1.00 = default. Needs PBR materials (High preset and up)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##decalmetal" ) ) { r_decalMetalMask.Reset(); }

	CvarSliderF( "Decals roughen surfaces", r_decalRoughen, 0.0f, 1.0f, "%.2f", "Dirt scatters light: a decal roughens the surface under it by its coverage, so highlights and "
		"reflections break up across stains - on metal and on glossy non-metals alike. "
		"0.00 = off, 1.00 = default. Needs PBR materials (High preset and up)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##decalrough" ) ) { r_decalRoughen.Reset(); }

	CvarSliderF( "Wet blood", r_decalWetBlood, 0.0f, 0.5f, "%.3f", "Blood decals glint off the lights around them as a wet film with its own relief - every blood decal, "
		"stock or mod, map-placed or splattered during play. Strength of the highlight; 0 = off (matte, "
		"stock-faithful blood), 0.057 = default. "
		"With a texture pack, it replaces the pack's own blood highlight "
		"(the pack's environment sheen still follows Blood decal sheen). Needs PBR materials." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##wetblood" ) ) { r_decalWetBlood.Reset(); }

	ImGui::BeginDisabled( r_decalWetBlood.GetFloat() <= 0.0f );
	CvarSliderF( "Wet blood relief", r_decalWetBloodRelief, 0.0f, 1.0f, "%.2f", "How strongly the film follows the blood's own height - bulging edges, ripples inside. Higher = the glint "
		"breaks into more sparkles, visible from more angles; 0.00 = a flat film that only glints at a light's exact "
		"mirror angle. 0.17 = default." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##bloodrelief" ) ) { r_decalWetBloodRelief.Reset(); }

	CvarSliderF( "Wet blood threshold", r_decalWetBloodThreshold, 0.0f, 0.95f, "%.2f", "How much blood there must be before it glints at all. Splat textures trail off into a mist too faint "
		"to see; raise this until the bare floor around a splat stops looking wet. The film feathers in just above "
		"the threshold. 0.30 = default." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##bloodfloor" ) ) { r_decalWetBloodThreshold.Reset(); }

	CvarSliderF( "Wet blood roughness", r_decalWetBloodRough, 0.05f, 0.8f, "%.2f", "Lower = tight, sharp glints (fresh blood); higher = a broad soft sheen (drying blood). 0.18 = default." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##bloodrough" ) ) { r_decalWetBloodRough.Reset(); }
	ImGui::EndDisabled();

	ImGui::EndDisabled();	// glass reflections (backend supported)
	EndSettingsGroup();
	}
}

static void DrawDbgGroup_SSAO()
{
	const bool supported = R_BackendSupportsEnhancements();
	// --- Ambient Occlusion. Master toggle is in Enhancements; the full tuning surface lives here,
	// split into the shared composite knobs, the screen-space (GTAO) producer, and the ray-traced
	// (RTAO) producer + its NRD denoiser. One producer runs at a time: RTAO (or RTGI) REPLACES GTAO. ---
	if ( BeginSettingsGroup( "Ambient Occlusion (SSAO / RTAO)" ) ) {
	ImGui::TextDisabled( "Occlude the ambient term in creases/contacts. Enable in Graphics > Ambient Occlusion." );
	ImGui::Spacing();

	ImGui::BeginDisabled( !supported );

	// ---- Common: the master switch + the composite knobs that apply to whichever producer runs. ----
	ImGui::SeparatorText( "Common" );

	CvarCheckbox( "AO (master)", r_ssao, "Master toggle (same cvar as Graphics > Ambient Occlusion). On = occlude the "
		"ambient term in creases/contacts; Off = vanilla flat ambient. Gates BOTH the screen-space "
		"and ray-traced producers below." );

	int ssaoDbg = r_ssaoDebug.GetInteger();
	const char *ssaoDbgItems[] = { "off (feed lighting)", "show AO buffer", "show bent normals", "show normal G-buffer" };
	if ( ImGui::Combo( "Debug View (r_ssaoDebug)", &ssaoDbg, ssaoDbgItems, IM_ARRAYSIZE( ssaoDbgItems ) ) ) {
		r_ssaoDebug.SetInteger( ssaoDbg );
	}
	AddTooltip( "Visualize the FINAL composited AO (either producer) over the scene. 1 = the occlusion "
		"buffer (white = lit, dark = occluded); 2 = the bent normals as RGB (GTAO); 3 = the raw normal "
		"G-buffer (GTAO, needs Normal Buffer on). Set back to 0 to feed lighting. For the raw/denoised "
		"RTAO stages use the RTAO Debug View below." );

	ImGui::BeginDisabled( !r_ssao.GetBool() );

	CvarSliderF( "Direct Light AO", r_ssaoDirectLight, 0.0f, 1.0f, "%.2f", "How strongly AO darkens direct (dynamic) light's diffuse. Doom 3 has almost no "
		"ambient, so THIS is what makes AO visible in normal gameplay. 1 = full, 0.75 = default, 0 = "
		"ambient-only (most faithful, but usually invisible here). Applies to whichever producer runs." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaodirect" ) ) { r_ssaoDirectLight.Reset(); }

	CvarSliderF( "Floor (min visibility)", r_ssaoFloor, 0.0f, 1.0f, "%.2f", "Anti-crush floor: how dark a fully-occluded spot may get. 0 = can reach black, "
		"0.03 = default, 1 = no darkening. Raise it if AO makes dark areas too murky to read." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaofloor" ) ) { r_ssaoFloor.Reset(); }

	CvarCheckbox( "Specular Occlusion", r_ssaoSpecular, "Also dim specular highlights in occluded areas on direct lights (stronger "
		"anti-plastic look; a mild deviation from vanilla). Scaled by Direct Light AO. Off by default." );

	ImGui::EndDisabled();	// !ssao (common sub-knobs)

	// ---- Screen-space GTAO: the horizon-march producer. Inert while RTAO or RTGI replaces it. ----
	ImGui::SeparatorText( "Screen-space (GTAO)" );
	if ( r_ssao.GetBool() && ( r_rtao.GetBool() || r_rtgi.GetBool() ) ) {
		ImGui::TextDisabled( r_rtgi.GetBool() ? "  (replaced by RT Global Illumination)" : "  (replaced by RT Ambient Occlusion)" );
	}
	ImGui::BeginDisabled( !r_ssao.GetBool() || r_rtao.GetBool() || r_rtgi.GetBool() );

	CvarSliderF( "Intensity (GTAO)", r_ssaoIntensity, 0.0f, 4.0f, "%.2f", "Strength of the screen-space darkening. 0 = none, 1.2 = default (compensates for the "
		"march), higher pushes the occlusion deeper. GTAO only - RTAO has its own Intensity below." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaoint" ) ) { r_ssaoIntensity.Reset(); }

	CvarSliderF( "Radius (world units)", r_ssaoRadius, 1.0f, 256.0f, "%.0f", "How far the occlusion samples reach, in world units. Small = tight contact creases "
		"only; large = broad, softer occlusion (and more expensive). Default 48." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaorad" ) ) { r_ssaoRadius.Reset(); }

	// Same discrete stops as Graphics > Ambient Occlusion > Resolution (one cvar, one selector style).
	const float ssaoResStops[]  = { 0.5f, 0.667f, 0.75f, 0.8f, 1.0f };
	const char *ssaoResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Four-fifths (4/5)", "Full" };
	CvarStopsSliderF( "Resolution Scale", r_ssaoResScale, ssaoResStops, ssaoResLabels, IM_ARRAYSIZE( ssaoResStops ),
		"GTAO buffer resolution as a fraction of the screen, bilaterally upscaled. The single biggest "
		"GTAO cost knob - AO is low-frequency, so Half reads nearly identical to Three-quarter. Lower "
		"this first if GTAO costs too much. Default Three-quarter. Same stops as Graphics > Resolution." );

	CvarSliderI( "Directions (slices)", r_ssaoSlices, 1, 8, "How many horizon-search directions per pixel. More = smoother, less directional "
		"noise. A big cost knob. Default 3. Check the cost with r_vkGpuTime 2 (the 'ao' scope)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaoslices" ) ) { r_ssaoSlices.Reset(); }

	CvarSliderI( "Steps per direction", r_ssaoSteps, 1, 12, "How many samples are marched along each direction (how finely each horizon is "
		"found). More = more accurate occlusion at range. Default 3." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaosteps" ) ) { r_ssaoSteps.Reset(); }

	CvarCheckbox( "Temporal-Funded Budget", r_ssaoTemporalTrade, "Preset lever: force 4 directions x 6 steps with Temporal Accumulation ON (overriding "
		"the sliders above) - cheaper than brute-force sampling for the same look, slightly softer. "
		"Preset-wired on for Medium/High/Ultra, off for Nightmare/Path Tracing (brute force)." );

	// Depth-mip acceleration + its two quality/speed knobs (docs/ssao-perf-optimization.md).
	CvarCheckbox( "Depth-Mip Acceleration##dev", r_ssaoDepthMip, "March the horizon search over a prefiltered linear-depth mip chain (far taps read "
		"coarse mips) instead of full-res depth every tap - cheaper at a wide radius. Same toggle as "
		"Graphics > Depth-Mip Acceleration." );

	ImGui::BeginDisabled( !r_ssaoDepthMip.GetBool() );
	CvarSliderF( "Mip Bias", r_ssaoDepthMipBias, 0.05f, 1.0f, "%.2f", "How eagerly the march drops to coarser mips: LOD = log2(stepPixels * this). Higher = "
		"coarser sooner (faster, but coarse depth smears occlusion across silhouettes into halos); "
		"lower = stays on finer mips (sharper, keeps most of the speedup). Default 0.1." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaomipbias" ) ) { r_ssaoDepthMipBias.Reset(); }

	CvarSliderI( "Mip Coarseness Cap", r_ssaoDepthMipMaxLod, 1, 5, "Caps how coarse the march may ever go. The coarsest mips (box-averaged depth) are "
		"where occlusion smears across silhouettes into halos, so a lower cap kills halos at a small "
		"speed cost; 5 = uncapped (old behaviour). Default 2." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaomipcap" ) ) { r_ssaoDepthMipMaxLod.Reset(); }
	ImGui::EndDisabled();

	CvarCheckbox( "Temporal Accumulation", r_ssaoTemporal, "Reuse the previous frame's AO (reprojected by camera motion) to amortize the "
		"horizon search across frames: smooths the per-pixel noise and lets Directions/Steps run "
		"lower for the same look. Ghosting on fast motion / disocclusion is bounded by a "
		"neighbourhood clamp." );

	ImGui::BeginDisabled( !r_ssaoTemporal.GetBool() );
	CvarSliderF( "Temporal Feedback", r_ssaoTemporalFeedback, 0.0f, 0.97f, "%.2f", "Fraction of the reprojected previous-frame AO kept each frame. Higher = smoother "
		"and steadier (and effectively cheaper) but more latency/ghosting; 0 = no accumulation. "
		"Default 0.90." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaofeedback" ) ) { r_ssaoTemporalFeedback.Reset(); }
	ImGui::EndDisabled();

	CvarCheckbox( "Normal Buffer (bump-mapped)", r_ssaoNormalBuffer, "Feed GTAO from a real bump-mapped normal G-buffer (an extra opaque geometry pass) "
		"instead of normals reconstructed from depth. Picks up normal-map surface detail and removes "
		"faceting (see Debug View 3), at the cost of one geometry pass. Off = reconstruct from depth "
		"(cheaper)." );

	ImGui::BeginDisabled( !r_ssaoNormalBuffer.GetBool() );
	CvarCheckbox( "Merge Normal into Prepass (VK)", r_ssaoMergeNormal, "Vulkan: produce the GTAO normal G-buffer (+ SSR rough/metal and velocity MRTs when "
		"wanted) INSIDE the depth prepass - one opaque geometry pass instead of two. Default on; off = "
		"the standalone normal prepass, kept as the A/B + the GL3 path. No visual change, pure cost." );
	ImGui::EndDisabled();

	CvarCheckbox( "Bent Normals", r_ssaoBentNormal, "Shade the AMBIENT light along the bent normal (average unoccluded direction) so it "
		"responds to macro occlusion, not just the surface normal. Visible in Debug View 2. Note: "
		"only bites where there IS ambient light, which is sparse in Doom 3, so the effect is subtle." );

	ImGui::BeginDisabled( !r_ssaoBentNormal.GetBool() );
	CvarSliderF( "Bent Strength", r_ssaoBentStrength, 0.0f, 1.0f, "%.2f", "How far the ambient lookup bends from the surface normal toward the bent normal. "
		"0 = surface normal (no effect), 1 = fully bent. Default 0.5." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##ssaobentstr" ) ) { r_ssaoBentStrength.Reset(); }
	ImGui::EndDisabled();

	ImGui::EndDisabled();	// GTAO (ssao on && !rtao && !rtgi)

	// ---- Ray-traced RTAO: the traced producer + its NRD ReBLUR denoiser. VK + RT hardware only. ----
	const bool rtCapable = R_SupportsRayTracing();
	ImGui::SeparatorText( rtCapable ? "Ray-traced (RTAO)" : "Ray-traced (RTAO) - needs Vulkan + RT GPU" );
	if ( rtCapable && r_ssao.GetBool() && r_rtgi.GetBool() ) {
		ImGui::TextDisabled( "  (RT Global Illumination supersedes RTAO - GI grounds the scene itself)" );
	}
	ImGui::BeginDisabled( !rtCapable || !r_ssao.GetBool() || r_rtgi.GetBool() );

	CvarCheckbox( "RT Ambient Occlusion##dbg", r_rtao, "Replace the screen-space GTAO with one traced hemisphere ray per pixel against the "
		"real scene, denoised by NRD ReBLUR across frames: sees occluders GTAO can't (off-screen, "
		"behind the camera), no screen-edge artifacts. Needs the AO master on + Motion Vectors or FSR "
		"(for the denoiser); enabling it builds the ray-tracing scene. Vulkan + RT hardware only." );

	ImGui::BeginDisabled( !r_rtao.GetBool() );

	CvarSliderF( "Ray Length (RTAO)", r_rtaoRadius, 8.0f, 512.0f, "%.0f", "How far each occlusion ray reaches, in world units. Longer rays gather broader, more "
		"grounded darkening from distant geometry than GTAO's screen-space radius can. Default 80." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##rtaorad" ) ) { r_rtaoRadius.Reset(); }

	CvarSliderF( "Intensity (RTAO)", r_rtaoIntensity, 0.0f, 4.0f, "%.2f", "RTAO strength (power curve on the traced visibility). The traced occlusion is already "
		"physically grounded, so the neutral 1.00 default shows it as-is. Raise for deeper creases." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##rtaoint" ) ) { r_rtaoIntensity.Reset(); }

	// Same discrete stops as Graphics > Ambient Occlusion > Resolution (RTAO) - one cvar, one selector style.
	const float rtaoResStops[]  = { 0.5f, 0.667f, 0.75f, 0.8f, 1.0f };
	const char *rtaoResLabels[] = { "Half (1/2)", "Two-thirds (2/3)", "Three-quarter (3/4)", "Four-fifths (4/5)", "Full" };
	CvarStopsSliderF( "Resolution Scale (RTAO)", r_rtaoResScale, rtaoResStops, rtaoResLabels, IM_ARRAYSIZE( rtaoResStops ),
		"Resolution RTAO runs the rays AND the NRD denoiser at, upscaled to full res. The denoiser is "
		"the bulk of RTAO's cost, so THIS is what saves time (unlike halving rays alone). AO is "
		"low-frequency, so Three-quarter / Half read nearly identical. Default Full; Path Tracing uses "
		"Three-quarter. Same stops as Graphics > Resolution." );

	int rtaoDbg = r_rtaoDebug.GetInteger();
	const char *rtaoDbgItems[] = { "off", "raw ray output", "denoised (pre-composite)" };
	if ( ImGui::Combo( "Debug View (r_rtaoDebug)", &rtaoDbg, rtaoDbgItems, IM_ARRAYSIZE( rtaoDbgItems ) ) ) {
		r_rtaoDebug.SetInteger( rtaoDbg );
	}
	AddTooltip( "Visualize the RTAO producer: 1 = the raw noisy 1-ray/px output (before NRD), 2 = the "
		"NRD-denoised occlusion (pre-composite). The final composited AO still shows in the Common "
		"Debug View above (1)." );

	// NRD ReBLUR denoiser knobs - the bulk of RTAO's cost and its temporal behaviour.
	ImGui::TextDisabled( "NRD ReBLUR denoiser" );

	CvarSliderI( "History Length", r_rtaoHistory, 0, 63, "Max frames NRD's ReBLUR accumulates for the 1-ray/px occlusion. 45 = the tuned default "
		"(clean with the least blur). 0 = NRD's own default (~30). Raise for smoother/less noise at "
		"the cost of more lag/trailing on moving occluders; lower (12-20) for snappier-but-noisier." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##rtaohist" ) ) { r_rtaoHistory.Reset(); }

	CvarSliderF( "Fast-History Clamp", r_rtaoFastClamp, 1.0f, 3.0f, "%.2f", "ReBLUR fastHistoryClampingSigmaScale: how tightly the slow main history is clamped to "
		"the responsive fast history. 2.0 = NRD default; NRD notes '1.1 is a safe value for occlusion' "
		"- the A/B for AO trailing behind moving occluders (NPCs, doors). Lower = snappier, noisier." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##rtaofastclamp" ) ) { r_rtaoFastClamp.Reset(); }

	CvarSliderI( "History-Fix Frames", r_rtaoHistoryFix, 0, 5, "ReBLUR historyFixFrameNum: frames of strong spatial reconstruction after a history "
		"reset (disocclusion). 3 = NRD default; the NRD sample uses 2 for occlusion. Must stay below "
		"the fast-history window (6)." );
	ImGui::SameLine();
	if ( ImGui::SmallButton( "reset##rtaohistfix" ) ) { r_rtaoHistoryFix.Reset(); }

	ImGui::EndDisabled();	// !rtao
	ImGui::EndDisabled();	// RTAO section (rtCapable && ssao && !rtgi)

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
		CvarCheckbox( "Baked Occlusion Maps (r_occlusionMaps)", r_occlusionMaps, "Per-material baked ambient-occlusion textures (the `occlusionmap` material "
			"stage), applied to the ambient and direct-light diffuse like SSAO. Inert unless a "
			"material ships an occlusion map; no stock Doom 3 asset does, so this targets mods." );

		ImGui::BeginDisabled( !r_occlusionMaps.GetBool() );
		CvarSliderF( "AO Map Strength", r_occlusionMapScale, 0.0f, 1.0f, "%.2f", "How strongly the occlusion map darkens the ambient term. 0 = off, 1 = the map "
			"at full darkening. Default 1.0." );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##oclscale" ) ) { r_occlusionMapScale.Reset(); }

		CvarSliderF( "Direct Light AO (map)", r_occlusionMapDirect, 0.0f, 1.0f, "%.2f", "How strongly the occlusion map darkens direct (dynamic) light's diffuse, as a "
			"fraction of AO Map Strength. Doom 3 is mostly dynamic light, so this is what makes the "
			"map visible; lower it if AO looks baked-in under moving lights. 0 = ambient-only. Default 0.9." );
		ImGui::SameLine();
		if ( ImGui::SmallButton( "reset##ocldirect" ) ) { r_occlusionMapDirect.Reset(); }
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
	DrawDbgGroup_ContactShadows();
	DrawDbgGroup_RTShadows();
	DrawDbgGroup_SSAO();
	DrawDbgGroup_RTGI();
	DrawDbgGroup_ReStirDI();
	DrawDbgGroup_OcclusionMaps();
	DrawDbgGroup_Tessellation();
	DrawDbgGroup_PBR();
	DrawDbgGroup_SkinSSS();
	DrawDbgGroup_Emissive();
	DrawDbgGroup_SSR();
	DrawDbgGroup_RTReflections();
	DrawDbgGroup_DepthOfField();
	DrawDbgGroup_EyeAdaptation();
	DrawDbgGroup_FSR();
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
//   dudePreset <n>   set the dude_preset cvar to raster tier n and apply it
//   dudePreset       apply whatever tier dude_preset currently holds
//
// The no-arg form is what the in-game selectors use: a choiceDef writes dude_preset (or dude_rtx)
// live, then fires "exec dudePreset" so we apply the just-written values. The ray-tracing axis is
// always read from dude_rtx (its own classic-menu selector / the ImGui tickbox write it); it is
// forced off when the device is not RT-capable, so a stale dude_rtx can never enable RT on a
// non-RT machine. Applying pins com_machineSpec to Ultra (3) - the legacy image-quality
// classification is no longer user-selectable (every GPU we support handles Ultra), so base
// texture/image quality stays maxed rather than leaving a stale low value behind.
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
	// Ray-tracing axis: dude_rtx, clamped off on a non-RT-capable device (Apply re-checks too).
	int rtMode = dude_rtx.GetInteger();
	if ( rtMode != RTX_OFF && !R_SupportsRayTracing() ) {
		rtMode = RTX_OFF;
	}
	ApplyEnhancementPreset( idx, rtMode );
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

// DUDE: the live ray-tracing axis state (RtMode: 0 = off, 1 = on), for the classic menu's
// Ray Tracing selector highlight (Session*.cpp pushes it into dude_rtx). Detected from the live
// RT cvars, same as the preset above.
int Com_DetectDudeRtx( void )
{
	int rtMode = RTX_OFF;
	DetectEnhancementPreset( &rtMode );
	return rtMode;
}

// DUDE: whether this device can run the ray-tracing axis at all (Vulkan + ray-query). The
// classic menu shows / hides its Ray Tracing row on this (gui::rtCapable).
bool Com_RtCapable( void )
{
	return R_SupportsRayTracing();
}

// DUDE: the in-game AA selector (mainmenu.gui) on the RHI backends. Three backend
// variants of the row are visibility-switched on "gui::dudeBackend" (0 = legacy ARB,
// keeps the MSAA r_multisamples row; 1 = opengl3, None/FXAA/SMAA; 2 = Vulkan, adds
// FSR2). The choiceDef binds the dude_aa bridge cvar; onAction runs "dudeAA", which
// applies it to the real archived cvars here.
int Com_DudeAABackend( void )
{
	if ( !glConfig.rhiBackend ) {
		return 0;						// legacy ARB: hardware MSAA is the only AA
	}
	return glConfig.coreProfile ? 1 : 2;	// GL core context = opengl3; RHI without one = Vulkan
}

// What the AA selector should currently show: FSR2 wins on Vulkan (it bypasses the
// post AA), otherwise the r_rhiAA mode.
int Com_DetectDudeAA( void )
{
	if ( Com_DudeAABackend() == 2 && r_fsr.GetBool() ) {
		return 3;
	}
	return idMath::ClampInt( 0, 2, r_rhiAA.GetInteger() );
}

void Com_DudeAA_f( const idCmdArgs &args )
{
	int v;
	if ( args.Argc() >= 2 ) {
		v = atoi( args.Argv( 1 ) );
		dude_aa.SetInteger( v );
	} else {
		v = dude_aa.GetInteger();		// written live by the menu choiceDef
	}
	Com_ApplyDudeAA( v );
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

int Com_DetectDudeRtx( void )
{
	return 0;
}

bool Com_RtCapable( void )
{
	return false;
}

#endif
