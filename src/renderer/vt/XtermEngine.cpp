// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "XtermEngine.hpp"
#include "../../types/inc/convert.hpp"
#pragma hdrstop
using namespace Microsoft::Console;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;

XtermEngine::XtermEngine(_In_ wil::unique_hfile hPipe,
                         const IDefaultColorProvider& colorProvider,
                         const Viewport initialViewport,
                         _In_reads_(cColorTable) const COLORREF* const ColorTable,
                         const WORD cColorTable,
                         const bool fUseAsciiOnly) :
    VtEngine(std::move(hPipe), colorProvider, initialViewport),
    _ColorTable(ColorTable),
    _cColorTable(cColorTable),
    _fUseAsciiOnly(fUseAsciiOnly),
    _usingUnderLine(false),
    _needToDisableCursor(false),
    _lastCursorIsVisible(false),
    _nextCursorIsVisible(true)
{
    // Set out initial cursor position to -1, -1. This will force our initial
    //      paint to manually move the cursor to 0, 0, not just ignore it.
    _lastText = VtEngine::INVALID_COORDS;
}

// Method Description:
// - Prepares internal structures for a painting operation. Turns the cursor
//      off, so we don't see it flashing all over the client's screen as we
//      paint the new contents.
// Arguments:
// - <none>
// Return Value:
// - S_OK if we started to paint. S_FALSE if we didn't need to paint. HRESULT
//      error code if painting didn't start successfully, or we failed to write
//      the pipe.
[[nodiscard]] HRESULT XtermEngine::StartPaint() noexcept
{
    RETURN_IF_FAILED(VtEngine::StartPaint());

    _trace.TraceLastText(_lastText);

    // Prep us to think that the cursor is not visible this frame. If it _is_
    // visible, then PaintCursor will be called, and we'll set this to true
    // during the frame.
    _nextCursorIsVisible = false;

    if (_firstPaint)
    {
        // MSFT:17815688
        // If the caller requested to inherit the cursor, we shouldn't
        //      clear the screen on the first paint. Otherwise, we'll clear
        //      the screen on the first paint, just to make sure that the
        //      terminal's state is consistent with what we'll be rendering.
        RETURN_IF_FAILED(_ClearScreen());
        _clearedAllThisFrame = true;
        _firstPaint = false;
    }
    else
    {
        const auto dirty = GetDirtyArea();

        // If we have 0 or 1 dirty pieces in the area, set as appropriate.
        Viewport dirtyView = dirty.empty() ? Viewport::Empty() : Viewport::FromInclusive(til::at(dirty, 0));

        // If there's more than 1, union them all up with the 1 we already have.
        for (size_t i = 1; i < dirty.size(); ++i)
        {
            dirtyView = Viewport::Union(dirtyView, Viewport::FromInclusive(til::at(dirty, i)));
        }
    }

    if (!_quickReturn)
    {
        if (_WillWriteSingleChar())
        {
            // Don't re-enable the cursor.
            _quickReturn = true;
        }
    }

    return S_OK;
}

// Routine Description:
// - EndPaint helper to perform the final rendering steps. Turn the cursor back
//      on.
// Arguments:
// - <none>
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for failing to allocate or write.
[[nodiscard]] HRESULT XtermEngine::EndPaint() noexcept
{
    // If during the frame we determined that the cursor needed to be disabled,
    //      then insert a cursor off at the start of the buffer, and re-enable
    //      the cursor here.
    if (_needToDisableCursor)
    {
        // If the cursor was previously visible, let's hide it for this frame,
        // by prepending a cursor off.
        if (_lastCursorIsVisible)
        {
            _buffer.insert(0, "\x1b[25l");
            _lastCursorIsVisible = false;
        }
        // If the cursor was NOT previously visible, then that's fine! we don't
        // need to worry, it's already off.
    }

    // If the cursor is moving from off -> on (including cases where we just
    // disabled if for this frame), show the cursor at the end of the frame
    if (_nextCursorIsVisible && !_lastCursorIsVisible)
    {
        RETURN_IF_FAILED(_ShowCursor());
    }
    // Otherwise, if the cursor previously was visible, and it should be hidden
    // (on -> off), hide it at the end of the frame.
    else if (!_nextCursorIsVisible && _lastCursorIsVisible)
    {
        RETURN_IF_FAILED(_HideCursor());
    }

    // Update our tracker of what we thought the last cursor state of the
    // terminal was.
    _lastCursorIsVisible = _nextCursorIsVisible;

    RETURN_IF_FAILED(VtEngine::EndPaint());

    _needToDisableCursor = false;

    return S_OK;
}

// Routine Description:
// - Write a VT sequence to either start or stop underlining text.
// Arguments:
// - legacyColorAttribute: A console attributes bit field containing information
//      about the underlining state of the text.
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for failing to allocate or write.
[[nodiscard]] HRESULT XtermEngine::_UpdateUnderline(const WORD legacyColorAttribute) noexcept
{
    bool textUnderlined = WI_IsFlagSet(legacyColorAttribute, COMMON_LVB_UNDERSCORE);
    if (textUnderlined != _usingUnderLine)
    {
        if (textUnderlined)
        {
            RETURN_IF_FAILED(_BeginUnderline());
        }
        else
        {
            RETURN_IF_FAILED(_EndUnderline());
        }
        _usingUnderLine = textUnderlined;
    }
    return S_OK;
}

// Routine Description:
// - Write a VT sequence to change the current colors of text. Only writes
//      16-color attributes.
// Arguments:
// - colorForeground: The RGB Color to use to paint the foreground text.
// - colorBackground: The RGB Color to use to paint the background of the text.
// - legacyColorAttribute: A console attributes bit field specifying the brush
//      colors we should use.
// - extendedAttrs - extended text attributes (italic, underline, etc.) to use.
// - isSettingDefaultBrushes: indicates if we should change the background color of
//      the window. Unused for VT
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for failing to allocate or write.
[[nodiscard]] HRESULT XtermEngine::UpdateDrawingBrushes(const COLORREF colorForeground,
                                                        const COLORREF colorBackground,
                                                        const WORD legacyColorAttribute,
                                                        const ExtendedAttributes extendedAttrs,
                                                        const bool /*isSettingDefaultBrushes*/) noexcept
{
    //When we update the brushes, check the wAttrs to see if the LVB_UNDERSCORE
    //      flag is there. If the state of that flag is different then our
    //      current state, change the underlining state.
    // We have to do this here, instead of in PaintBufferGridLines, because
    //      we'll have already painted the text by the time PaintBufferGridLines
    //      is called.
    // TODO:GH#2915 Treat underline separately from LVB_UNDERSCORE
    RETURN_IF_FAILED(_UpdateUnderline(legacyColorAttribute));
    // The base xterm mode only knows about 16 colors
    return VtEngine::_16ColorUpdateDrawingBrushes(colorForeground,
                                                  colorBackground,
                                                  WI_IsFlagSet(extendedAttrs, ExtendedAttributes::Bold),
                                                  _ColorTable,
                                                  _cColorTable);
}

// Routine Description:
// - Draws the cursor on the screen
// Arguments:
// - options - Options that affect the presentation of the cursor
// Return Value:
// - S_OK or suitable HRESULT error from writing pipe.
[[nodiscard]] HRESULT XtermEngine::PaintCursor(const IRenderEngine::CursorOptions& options) noexcept
{
    // PaintCursor is only called when the cursor is in fact visible in a single
    // frame. When this is called, mark _nextCursorIsVisible as true. At the end
    // of the frame, we'll decide to either turn the cursor on or not, based
    // upon the previous state.

    // When this method is not called during a frame, it's because the cursor
    // was not visible. In that case, at the end of the frame,
    // _nextCursorIsVisible will still be false (from when we set it during
    // StartPaint)
    _nextCursorIsVisible = true;
    return VtEngine::PaintCursor(options);
}

// Routine Description:
// - Write a VT sequence to move the cursor to the specified coordinates. We
//      also store the last place we left the cursor for future optimizations.
//  If the cursor only needs to go to the origin, only write the home sequence.
//  If the new cursor is only down one line from the current, only write a newline
//  If the new cursor is only down one line and at the start of the line, write
//      a carriage return.
//  Otherwise just write the whole sequence for moving it.
// Arguments:
// - coord: location to move the cursor to.
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for failing to allocate or write.
[[nodiscard]] HRESULT XtermEngine::_MoveCursor(COORD const coord) noexcept
{
    HRESULT hr = S_OK;

    _trace.TraceMoveCursor(_lastText, coord);

    if (coord.X != _lastText.X || coord.Y != _lastText.Y)
    {
        if (coord.X == 0 && coord.Y == 0)
        {
            _needToDisableCursor = true;
            hr = _CursorHome();
        }
        else if (_resized && _resizeQuirk)
        {
            hr = _CursorPosition(coord);
        }
        else if (coord.X == 0 && coord.Y == (_lastText.Y + 1))
        {
            // Down one line, at the start of the line.

            // If the previous line wrapped, then the cursor is already at this
            //      position, we just don't know it yet. Don't emit anything.
            bool previousLineWrapped = false;
            if (_wrappedRow.has_value())
            {
                previousLineWrapped = coord.Y == _wrappedRow.value() + 1;
            }

            if (previousLineWrapped)
            {
                _trace.TraceWrapped();
                hr = S_OK;
            }
            else
            {
                std::string seq = "\r\n";
                hr = _Write(seq);
            }
        }
        else if (_delayedEolWrap)
        {
            // GH#1245, GH#357 - If we were in the delayed EOL wrap state, make
            // sure to _manually_ position the cursor now, with a full CUP
            // sequence, don't try and be clever with \b or \r or other control
            // sequences. Different terminals (conhost, gnome-terminal, wt) all
            // behave differently with how the cursor behaves at an end of line.
            // This is the only solution that works in all of them, and also
            // works wrapped lines emitted by conpty.
            //
            // Make sure to do this _after_ the possible \r\n branch above,
            // otherwise we might accidentally break wrapped lines (GH#405)
            hr = _CursorPosition(coord);
        }
        else if (coord.X == 0 && coord.Y == _lastText.Y)
        {
            // Start of this line
            std::string seq = "\r";
            hr = _Write(seq);
        }
        else if (coord.X == _lastText.X && coord.Y == (_lastText.Y + 1))
        {
            // Down one line, same X position
            std::string seq = "\n";
            hr = _Write(seq);
        }
        else if (coord.X == (_lastText.X - 1) && coord.Y == (_lastText.Y))
        {
            // Back one char, same Y position
            std::string seq = "\b";
            hr = _Write(seq);
        }
        else if (coord.Y == _lastText.Y && coord.X > _lastText.X)
        {
            // Same line, forward some distance
            short distance = coord.X - _lastText.X;
            hr = _CursorForward(distance);
        }
        else
        {
            _needToDisableCursor = true;
            hr = _CursorPosition(coord);
        }

        if (SUCCEEDED(hr))
        {
            _lastText = coord;
        }
    }
    if (_lastText.Y != _lastViewport.ToOrigin().BottomInclusive())
    {
        _newBottomLine = false;
    }
    _deferredCursorPos = INVALID_COORDS;

    _wrappedRow = std::nullopt;

    _delayedEolWrap = false;

    return hr;
}

// Routine Description:
// - Scrolls the existing data on the in-memory frame by the scroll region
//      deltas we have collectively received through the Invalidate methods
//      since the last time this was called.
//  Move the cursor to the origin, and insert or delete rows as appropriate.
//      The inserted rows will be blank, but marked invalid by InvalidateScroll,
//      so they will later be written by PaintBufferLine.
// Arguments:
// - <none>
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for failing to allocate or write.
[[nodiscard]] HRESULT XtermEngine::ScrollFrame() noexcept
try
{
    if (_scrollDelta.x() != 0)
    {
        // No easy way to shift left-right. Everything needs repainting.
        return InvalidateAll();
    }
    if (_scrollDelta.y() == 0)
    {
        // There's nothing to do here. Do nothing.
        return S_OK;
    }

    const short dy = _scrollDelta.y<SHORT>();
    const short absDy = static_cast<short>(abs(dy));

    HRESULT hr = S_OK;
    if (dy < 0)
    {
        // Instead of deleting the first line (causing everything to move up)
        // move to the bottom of the buffer, and newline.
        //      That will cause everything to move up, by moving the viewport down.
        // This will let remote conhosts scroll up to see history like normal.
        const short bottom = _lastViewport.ToOrigin().BottomInclusive();
        hr = _MoveCursor({ 0, bottom });
        if (SUCCEEDED(hr))
        {
            std::string seq = std::string(absDy, '\n');
            hr = _Write(seq);
            // Mark that the bottom line is new, so we won't spend time with an
            // ECH on it.
            _newBottomLine = true;
        }
        // We don't need to _MoveCursor the cursor again, because it's still
        //      at the bottom of the viewport.
    }
    else if (dy > 0)
    {
        // Move to the top of the buffer, and insert some lines of text, to
        //      cause the viewport contents to shift down.
        hr = _MoveCursor({ 0, 0 });
        if (SUCCEEDED(hr))
        {
            hr = _InsertLine(absDy);
        }
    }

    return hr;
}
CATCH_RETURN();

// Routine Description:
// - Notifies us that the console is attempting to scroll the existing screen
//      area. Add the top or bottom rows to the invalid region, and update the
//      total scroll delta accumulated this frame.
// Arguments:
// - pcoordDelta - Pointer to character dimension (COORD) of the distance the
//      console would like us to move while scrolling.
// Return Value:
// - S_OK if we succeeded, else an appropriate HRESULT for safemath failure
[[nodiscard]] HRESULT XtermEngine::InvalidateScroll(const COORD* const pcoordDelta) noexcept
try
{
    const til::point delta{ *pcoordDelta };

    if (delta != til::point{ 0, 0 })
    {
        _trace.TraceInvalidateScroll(delta);

        // Scroll the current offset and invalidate the revealed area
        _invalidMap.translate(delta, true);

        _scrollDelta += delta;
    }

    return S_OK;
}
CATCH_RETURN();

// Routine Description:
// - Draws one line of the buffer to the screen. Writes the characters to the
//      pipe, encoded in UTF-8 or ASCII only, depending on the VtIoMode.
//      (See descriptions of both implementations for details.)
// Arguments:
// - clusters - text and column counts for each piece of text.
// - coord - character coordinate target to render within viewport
// - trimLeft - This specifies whether to trim one character width off the left
//      side of the output. Used for drawing the right-half only of a
//      double-wide character.
// - lineWrapped: true if this run we're painting is the end of a line that
//   wrapped. If we're not painting the last column of a wrapped line, then this
//   will be false.
// Return Value:
// - S_OK or suitable HRESULT error from writing pipe.
[[nodiscard]] HRESULT XtermEngine::PaintBufferLine(std::basic_string_view<Cluster> const clusters,
                                                   const COORD coord,
                                                   const bool /*trimLeft*/,
                                                   const bool lineWrapped) noexcept
{
    return _fUseAsciiOnly ?
               VtEngine::_PaintAsciiBufferLine(clusters, coord) :
               VtEngine::_PaintUtf8BufferLine(clusters, coord, lineWrapped);
}

// Method Description:
// - Wrapper for ITerminalOutputConnection. Write either an ascii-only, or a
//      proper utf-8 string, depending on our mode.
// Arguments:
// - wstr - wstring of text to be written
// Return Value:
// - S_OK or suitable HRESULT error from either conversion or writing pipe.
[[nodiscard]] HRESULT XtermEngine::WriteTerminalW(const std::wstring_view wstr) noexcept
{
    RETURN_IF_FAILED(_fUseAsciiOnly ?
                         VtEngine::_WriteTerminalAscii(wstr) :
                         VtEngine::_WriteTerminalUtf8(wstr));
    // GH#4106, GH#2011 - WriteTerminalW is only ever called by the
    // StateMachine, when we've encountered a string we don't understand. When
    // this happens, we usually don't actually trigger another frame, but we
    // _do_ want this string to immediately be sent to the terminal. Since we
    // only flush our buffer on actual frames, this means that strings we've
    // decided to pass through would have gotten buffered here until the next
    // actual frame is triggered.
    //
    // To fix this, flush here, so this string is sent to the connected terminal
    // application.

    return _Flush();
}

// Method Description:
// - Updates the window's title string. Emits the VT sequence to SetWindowTitle.
// Arguments:
// - newTitle: the new string to use for the title of the window
// Return Value:
// - S_OK
[[nodiscard]] HRESULT XtermEngine::_DoUpdateTitle(const std::wstring& newTitle) noexcept
{
    // inbox telnet uses xterm-ascii as its mode. If we're in ascii mode, don't
    //      do anything, to maintain compatibility.
    if (_fUseAsciiOnly)
    {
        return S_OK;
    }

    try
    {
        const auto converted = ConvertToA(CP_UTF8, newTitle);
        return VtEngine::_ChangeTitle(converted);
    }
    CATCH_RETURN();
}
