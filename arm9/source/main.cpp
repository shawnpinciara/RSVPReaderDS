// SPDX-License-Identifier: CC0-1.0
// RSVPReaderDS — Rapid Serial Visual Presentation book reader for Nintendo DS.
//
// Top screen modes:
//   RSVP mode   — single word, ORP letter red+underlined, centred on ORP,
//                 Batang14 scaled up to user-chosen max (1-4×, auto-shrinks
//                 for long words).
//   Browse mode — 3 lines of book context at native scale; current word red;
//                 active while holding D-pad Left/Right.
//
// Controls:
//   A              — Play / Pause  (or load highlighted book when none loaded)
//   B              — Rewind to start of current sentence
//   L              — Decrease WPM (−25, min 50)
//   R              — Increase WPM (+25, max 1000)
//   D-pad Up/Down  — Jump ±10 words
//   D-pad L/R      — Step 1 word; hold for continuous scroll + browse view
//   Y              — Toggle dark / light theme
//   X              — Font size −1×
//   Start          — Save state to /book/.state
//   Select         — Load highlighted book
//   Start + Select — Quit

#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <framebuffer.h>
#include <woopsi.h>
#include <amigascreen.h>
#include <amigawindow.h>
#include <button.h>
#include <label.h>
#include <listbox.h>
#include <hardware.h>
#include <stylus.h>
#include <gadgeteventhandler.h>
#include "fonts/batang14.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

using namespace WoopsiUI;

// ── GadgetCallback ─────────────────────────────────────────────────────────────

class GadgetCallback : public GadgetEventHandler {
public:
    using Fn = std::function<void(Gadget&)>;

    GadgetCallback(Fn onAction = nullptr, Fn onValueChange = nullptr,
                   Fn onClick  = nullptr, Fn onRelease    = nullptr)
        : _onAction(std::move(onAction)), _onValueChange(std::move(onValueChange)),
          _onClick(std::move(onClick)),   _onRelease(std::move(onRelease)) {}

    void handleActionEvent(Gadget& g) override      { if (_onAction)      _onAction(g);      }
    void handleValueChangeEvent(Gadget& g) override { if (_onValueChange) _onValueChange(g); }
    void handleClickEvent(Gadget& g, const WoopsiPoint&) override   { if (_onClick)  _onClick(g);  }
    void handleReleaseEvent(Gadget& g, const WoopsiPoint&) override { if (_onRelease) _onRelease(g); }

private:
    Fn _onAction, _onValueChange, _onClick, _onRelease;
};

// ── WordCanvas ─────────────────────────────────────────────────────────────────
// Dual-mode full-screen word display.
//
// RSVP mode: one word, Batang14 scaled up to _maxScale× (falls back for long
// words), ORP letter red + underlined, horizontally pivoted on ORP centre.
//
// Browse mode: 3 lines of book text at native Batang14 scale, word-wrapped to
// screen width, current word drawn in red.

class WordCanvas : public Gadget {

    // ── RSVP state ─────────────────────────────────────────────────────────────
    WoopsiString _word;
    int          _maxScale  = 3;    // user-chosen ceiling; auto-shrinks per word

    // ── Browse state ───────────────────────────────────────────────────────────
    bool                            _browseMode = false;
    const std::vector<std::string>* _wordList   = nullptr;
    int                             _browseIdx  = 0;
    int                             _prevBrowseIdx  = -1; // word highlighted on last draw
    int                             _prevFirstLine  = -1; // first visible line on last draw

    // ── Shared ─────────────────────────────────────────────────────────────────
    u16      _bgColour;
    u16      _fgColour;
    Batang14 _font;

    static const int kBufW = 64;
    static const int kBufH = 20;
    static u16       _charBuf[kBufW * kBufH];

    // ── Helpers ────────────────────────────────────────────────────────────────

    int orpIndex() const {
        int n = (int)_word.getLength();
        if (n <= 2) return 0;
        return (n * 35) / 100;
    }

    int computeScale() const {
        if (_word.getLength() == 0) return _maxScale;
        int w = 0;
        for (s32 i = 0; i < _word.getLength(); i++)
            w += (int)_font.getCharWidth(_word.getCharAt(i));
        for (int s = _maxScale; s >= 1; s--)
            if (w * s <= (int)getWidth() - 8) return s;
        return 1;
    }

    // Render one glyph from _charBuf into port at (xCur, y0) scaled by `scale`,
    // using horizontal run-length batching. fg is the foreground colour.
    void blitScaled(GraphicsPort* port, u8 cw, u8 fontH, int scale,
                    s16 xCur, s16 y0, u16 fg) const {
        for (int py = 0; py < (int)fontH; py++) {
            int runStart = -1;
            for (int px = 0; px <= (int)cw; px++) {
                const bool lit = (px < (int)cw) &&
                                 (_charBuf[py * kBufW + px] != _bgColour);
                if (lit && runStart < 0) {
                    runStart = px;
                } else if (!lit && runStart >= 0) {
                    port->drawFilledRect(
                        xCur + (s16)(runStart * scale),
                        y0   + (s16)(py * scale),
                        (u16)((px - runStart) * scale), (u16)scale, fg);
                    runStart = -1;
                }
            }
        }
    }

    // ── RSVP rendering ─────────────────────────────────────────────────────────

    void drawRSVP(GraphicsPort* port) {
        const u16 kRed = woopsiRGB(31, 0, 0);

        port->drawFilledRect(0, 0, getWidth(), getHeight(), _bgColour);
        if (_word.getLength() == 0) return;

        const u8  fontH = _font.getHeight();
        const int scale = computeScale();
        const int orp   = orpIndex();

        // Pivot the word so the ORP letter's centre is at screen centre (x=128).
        int leftW = 0, orpW = 0;
        for (s32 i = 0; i < _word.getLength(); i++) {
            int cw_i = (int)_font.getCharWidth(_word.getCharAt(i)) * scale;
            if      (i < orp)  leftW += cw_i;
            else if (i == orp) orpW   = cw_i;
        }
        const s16 x0 = (s16)(getWidth() / 2 - leftW - orpW / 2);
        const s16 y0 = (s16)((getHeight() - (int)fontH * scale - 3) / 2);

        s16 xCur = x0;
        for (s32 i = 0; i < _word.getLength(); i++) {
            const u32 ch    = _word.getCharAt(i);
            const u8  cw    = _font.getCharWidth(ch);
            const bool isOrp= (i == (s32)orp);
            const u16 fg    = isOrp ? kRed : _fgColour;

            // Render glyph into static buffer
            FrameBuffer fb(_charBuf, (u16)kBufW, (u16)fontH);
            fb.blitFill(0, 0, _bgColour, (u32)kBufW * (u32)fontH);
            _font.drawChar(&fb, ch, fg, 0, 0, 0, 0, (u16)(cw - 1), (u16)(fontH - 1));

            blitScaled(port, cw, fontH, scale, xCur, y0, fg);

            if (isOrp) {
                const s16 ulY = y0 + (s16)((int)fontH * scale) + 2;
                port->drawFilledRect(xCur, ulY, (u16)((int)cw * scale), 2, kRed);
            }
            xCur += (s16)((int)cw * scale);
        }
    }

    // ── Browse rendering ────────────────────────────────────────────────────────
    // Fills the full screen with word-wrapped book context.  The current word
    // is drawn in red; all other text uses _fgColour.
    //
    // Uses GraphicsPort::drawText for text — one call per word, orders of
    // magnitude faster than the per-character FrameBuffer path used in RSVP
    // mode.  Combines two strategies to eliminate flickering:
    //
    //   • Full redraw (first draw or line window shifted): single DMA bg-fill
    //     followed by drawText for every visible word.
    //   • Partial update (same lines, only the highlighted word changed): only
    //     the previously- and newly-highlighted words are cleared and redrawn
    //     — typically 4 Woopsi calls total.

    void drawBrowse(GraphicsPort* port) {
        const u16 kRed  = woopsiRGB(31, 0, 0);
        const u8  fontH = _font.getHeight();
        const int lineH = (int)fontH + 3;          // 3-px leading between lines
        const int spW   = (int)_font.getCharWidth((u32)' ');
        const int W     = getWidth();
        const int H     = getHeight();
        const int N     = (int)_wordList->size();
        const int kShow = H / lineH;               // lines that fit on screen

        if (kShow < 1 || N == 0) return;

        // ── Word-wrap layout ──────────────────────────────────────────────────
        const int scanStart = std::max(0, _browseIdx - 300);
        const int scanEnd   = std::min(N, _browseIdx + 300);

        std::vector<int> lineEnds;  // lineEnds[l] = exclusive end of line l
        lineEnds.reserve(kShow * 3);
        int lw = 0;
        for (int i = scanStart; i < scanEnd; i++) {
            int ww = 0;
            for (unsigned char c : (*_wordList)[i]) ww += (int)_font.getCharWidth((u32)c);
            if (lw > 0 && lw + spW + ww > W) { lineEnds.push_back(i); lw = ww; }
            else                              { lw += (lw > 0 ? spW : 0) + ww; }
        }
        lineEnds.push_back(scanEnd);

        // ── Find current line ─────────────────────────────────────────────────
        int curLine = 0, ls = scanStart;
        for (int l = 0; l < (int)lineEnds.size(); l++) {
            if (_browseIdx >= ls && _browseIdx < lineEnds[l]) { curLine = l; break; }
            ls = lineEnds[l];
        }

        // ── Choose visible window (current line centred) ───────────────────────
        int first = std::max(0, curLine - kShow / 2);
        int last  = std::min((int)lineEnds.size() - 1, first + kShow - 1);
        first     = std::max(0, last - kShow + 1);
        const int visLines = last - first + 1;
        const s16 yBase    = (s16)((H - visLines * lineH) / 2);

        // ── Diff with previous draw ───────────────────────────────────────────
        const int prevFirst = _prevFirstLine;
        const int prevIdx   = _prevBrowseIdx;
        _prevFirstLine = first;
        _prevBrowseIdx = _browseIdx;

        const bool firstDraw  = (prevFirst < 0);
        const bool viewShifted= firstDraw || (first != prevFirst);

        if (viewShifted) {
            // Full clear — single DMA fill, very fast
            port->drawFilledRect(0, 0, (u16)W, (u16)H, _bgColour);
        }

        // ── Render visible lines ──────────────────────────────────────────────
        ls = scanStart;
        for (int l = 0; l < (int)lineEnds.size(); l++) {
            const int lEnd = lineEnds[l];
            if (l >= first && l <= last) {
                const s16 lineY = yBase + (s16)((l - first) * lineH);
                s16 x = 0;
                for (int wi = ls; wi < lEnd; wi++) {
                    if (wi > ls) x += (s16)spW;

                    // Measure word pixel width (needed for x advancement and
                    // for the partial-update background clear).
                    int ww = 0;
                    for (unsigned char c : (*_wordList)[wi])
                        ww += (int)_font.getCharWidth((u32)c);

                    const bool wasHl   = (!firstDraw && wi == prevIdx);
                    const bool isHl    = (wi == _browseIdx);
                    const bool needDraw= viewShifted || wasHl || isHl;

                    if (needDraw) {
                        const u16 fg = isHl ? kRed : _fgColour;
                        if (!viewShifted) {
                            // Partial update: clear only this word's rect so
                            // the colour change doesn't ghost over old pixels.
                            port->drawFilledRect(x, lineY, (u16)ww, (u16)fontH, _bgColour);
                        }
                        // drawText renders transparent background — one call
                        // per word, internally optimised by Woopsi.
                        port->drawText(x, lineY, &_font,
                                       WoopsiString((*_wordList)[wi].c_str()),
                                       0, (s32)(*_wordList)[wi].size(), fg);
                    }
                    x += (s16)ww;
                }
            }
            ls = lEnd;
            if (l > last) break;
        }
    }

protected:
    void drawContents(GraphicsPort* port) override {
        if (_browseMode && _wordList && !_wordList->empty())
            drawBrowse(port);
        else
            drawRSVP(port);
    }

    void drawBorder(GraphicsPort*) override {}

public:
    WordCanvas(s16 x, s16 y, u16 w, u16 h)
        : Gadget(x, y, w, h),
          _bgColour(woopsiRGB(0, 0, 0)),
          _fgColour(woopsiRGB(31, 31, 31)) {}

    // RSVP mode
    void setWord(const WoopsiString& w) { _word = w; markRectsDamaged(); }

    // Browse mode
    void setBrowse(bool on, const std::vector<std::string>* words, int idx) {
        _browseMode    = on;
        _wordList      = words;
        _browseIdx     = idx;
        _prevBrowseIdx = -1;   // force full redraw on first browse frame
        _prevFirstLine = -1;
        markRectsDamaged();
    }
    void setBrowseIdx(int idx) {
        if (_browseIdx != idx) { _browseIdx = idx; markRectsDamaged(); }
    }

    // Font size
    void setMaxScale(int s) {
        _maxScale = std::max(1, std::min(4, s));
        if (!_browseMode) markRectsDamaged();
    }
    int getMaxScale() const { return _maxScale; }

    // Theme
    void setTheme(bool lightMode) {
        _bgColour = lightMode ? woopsiRGB(31, 31, 31) : woopsiRGB(0, 0, 0);
        _fgColour = lightMode ? woopsiRGB(0, 0, 0)   : woopsiRGB(31, 31, 31);
        markRectsDamaged();
    }

    virtual inline ~WordCanvas() {}
};

u16 WordCanvas::_charBuf[WordCanvas::kBufW * WordCanvas::kBufH];

// ── RSVPReaderApp ──────────────────────────────────────────────────────────────

class RSVPReaderApp : public Woopsi {

    // ── UI ─────────────────────────────────────────────────────────────────────
    AmigaScreen* _topScreen     = nullptr;
    AmigaScreen* _bottomScreen  = nullptr;
    WordCanvas*  _wordCanvas    = nullptr;
    Label*       _bookLabel     = nullptr;
    Label*       _wpmLabel      = nullptr;
    Label*       _fontSizeLabel = nullptr;
    Label*       _progressLabel = nullptr;
    Label*       _statusLabel   = nullptr;
    ListBox*     _bookList      = nullptr;
    Button*      _themeBtn      = nullptr;

    // ── State ──────────────────────────────────────────────────────────────────
    std::vector<std::string> _words;
    int         _currentWord   = 0;
    int         _wpm           = 300;
    bool        _playing       = false;
    bool        _lightMode     = false;
    bool        _browseMode    = false;   // true while D-pad L/R is held
    u32         _wordStartVBL  = 0;
    u32         _lrHeldSince   = 0;      // frame when L/R was first pressed
    u32         _lrLastRepeat  = 0;      // frame of last repeat step
    u32         _browseLingerEnd = 0;    // frame at which to exit browse (0=inactive)
    std::string _currentBookPath;
    std::vector<std::string> _bookPaths;

    // ── Timing ────────────────────────────────────────────────────────────────

    int baseIntervalMs() const { return 60000 / _wpm; }

    int pacingBonusMs(const std::string& word) const {
        int base = baseIntervalMs(), bonus = 0;
        int len  = (int)word.size();
        if (len > 6)  bonus += base * (len - 6)  * 6 / 100;
        if (len > 10) bonus += base * (len - 10)  * 3 / 100;
        bonus = std::min(bonus, base * 170 / 100);
        if (!word.empty()) {
            int pct  = 0;
            char last = word.back();
            if      (last == ',')                           pct = 45;
            else if (last == '-')                           pct = 60;
            else if (last == ';' || last == ':')            pct = 80;
            else if (last == '.' || last == '!' || last == '?') pct = 135;
            bonus += std::min(base * pct / 100, base * 135 / 100);
        }
        return bonus;
    }

    int wordDurationMs() const {
        int ms = baseIntervalMs();
        if (_currentWord < (int)_words.size())
            ms += pacingBonusMs(_words[_currentWord]);
        return ms;
    }

    // ── UI helpers ─────────────────────────────────────────────────────────────

    void updateWpmLabel() {
        char buf[24]; snprintf(buf, sizeof(buf), "WPM: %d", _wpm);
        _wpmLabel->setText(WoopsiString(buf));
    }

    void updateFontSizeLabel() {
        char buf[6]; snprintf(buf, sizeof(buf), "%dx", _wordCanvas->getMaxScale());
        _fontSizeLabel->setText(WoopsiString(buf));
    }

    void updateStatus(const char* msg) {
        _statusLabel->setText(WoopsiString(msg));
    }

    void showWord(int index) {
        if (_words.empty()) return;
        index = std::max(0, std::min(index, (int)_words.size() - 1));
        _currentWord  = index;
        _wordStartVBL = _vblCount;

        if (_browseMode)
            _wordCanvas->setBrowseIdx(_currentWord);
        else
            _wordCanvas->setWord(WoopsiString(_words[index].c_str()));

        char buf[56];
        int total = (int)_words.size();
        int pct   = total > 1 ? _currentWord * 100 / (total - 1) : 100;
        snprintf(buf, sizeof(buf), "%d%%  %d / %d", pct, _currentWord + 1, total);
        _progressLabel->setText(WoopsiString(buf));
    }

    void advanceWord() {
        int next = _currentWord + 1;
        if (next >= (int)_words.size()) {
            _playing = false;
            updateStatus("Done!");
        } else {
            showWord(next);
        }
    }

    void rewindSentence() {
        if (_currentWord <= 1) { showWord(0); return; }
        for (int i = _currentWord - 1; i > 0; i--) {
            const std::string& w = _words[i];
            if (!w.empty()) {
                char last = w.back();
                if (last == '.' || last == '!' || last == '?') { showWord(i + 1); return; }
            }
        }
        showWord(0);
    }

    void toggleTheme() {
        _lightMode = !_lightMode;
        _wordCanvas->setTheme(_lightMode);
        _themeBtn->setText(_lightMode ? WoopsiString("Dark") : WoopsiString("Light"));
    }

    // Enter browse (context) mode: show 3-line book view around current word.
    // RSVP auto-advance is suspended while in browse mode.
    void enterBrowse() {
        _browseMode = true;
        _wordCanvas->setBrowse(true, &_words, _currentWord);
    }

    // Exit browse mode: return to single-word RSVP display and resume if playing.
    void exitBrowse() {
        _browseMode = false;
        _wordCanvas->setBrowse(false, nullptr, 0);
        if (!_words.empty())
            _wordCanvas->setWord(WoopsiString(_words[_currentWord].c_str()));
        if (_playing) _wordStartVBL = _vblCount;   // restart timing from new position
    }

    // ── Book management ────────────────────────────────────────────────────────

    void consolidatePunctuation() {
        std::vector<std::string> out;
        out.reserve(_words.size());
        for (size_t i = 0; i < _words.size(); i++) {
            const std::string& w = _words[i];
            bool hasAlnum = false;
            for (unsigned char c : w) { if (isalnum(c)) { hasAlnum = true; break; } }
            if (hasAlnum) {
                out.push_back(w);
            } else {
                const char first    = w.empty() ? 0 : w[0];
                const bool isOpening = (first == '(' || first == '[' || first == '{');
                if (isOpening && i + 1 < _words.size())
                    _words[i + 1] = w + _words[i + 1];
                else if (!out.empty())
                    out.back() += w;
                else if (i + 1 < _words.size())
                    _words[i + 1] = w + _words[i + 1];
            }
        }
        _words = std::move(out);
    }

    bool loadBook(const char* path) {
        FILE* f = fopen(path, "r");
        if (!f) return false;
        _words.clear();
        _currentBookPath = path;
        _playing = false;
        static const int kMax = 100000;
        char line[512]; bool first = true;
        while (fgets(line, sizeof(line), f)) {
            const char* p = line;
            if (first) {
                if ((u8)p[0]==0xEF && (u8)p[1]==0xBB && (u8)p[2]==0xBF) p += 3;
                first = false;
            }
            while (*p && (int)_words.size() < kMax) {
                while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
                if (!*p) break;
                const char* s = p;
                while (*p && *p!=' '&&*p!='\t'&&*p!='\r'&&*p!='\n') p++;
                if (p > s) {
                    std::string w; w.reserve((size_t)(p - s));
                    for (const char* c = s; c < p; c++)
                        if ((u8)*c >= 32 && (u8)*c < 128) w += *c;
                    if (!w.empty()) _words.push_back(std::move(w));
                }
            }
            if ((int)_words.size() >= kMax) break;
        }
        fclose(f);
        consolidatePunctuation();
        return !_words.empty();
    }

    void loadAndShowBook(const char* path) {
        updateStatus("Loading...");
        if (loadBook(path)) {
            const char* slash = strrchr(path, '/');
            const char* name  = slash ? slash + 1 : path;
            char buf[64]; snprintf(buf, sizeof(buf), "%.60s", name);
            _bookLabel->setText(WoopsiString(buf));
            showWord(0);
            updateStatus("A to play");
        } else {
            updateStatus("Load failed!");
        }
    }

    void saveState() const {
        FILE* f = fopen("/book/.state", "w");
        if (!f) return;
        fprintf(f, "%s\n%d\n%d\n", _currentBookPath.c_str(), _currentWord, _wpm);
        fclose(f);
    }

    bool loadSavedState() {
        FILE* f = fopen("/book/.state", "r");
        if (!f) return false;
        char path[256] = {}; int word = 0, wpm = 300;
        bool ok = fgets(path, sizeof(path), f) != nullptr;
        if (ok) {
            int n = (int)strlen(path);
            if (n > 0 && path[n-1] == '\n') path[--n] = 0;
            ok = (n > 0) && (fscanf(f, "%d\n%d", &word, &wpm) == 2);
        }
        fclose(f);
        if (!ok) return false;
        _currentBookPath = path; _currentWord = word; _wpm = wpm;
        return true;
    }

    static bool hasTxt(const char* n) {
        int l = (int)strlen(n);
        return l > 4 && n[l-4]=='.' &&
               tolower((u8)n[l-3])=='t' && tolower((u8)n[l-2])=='x' && tolower((u8)n[l-1])=='t';
    }

    std::vector<std::string> discoverBooks() const {
        std::vector<std::string> v;
        DIR* d = opendir("/book");
        if (!d) return v;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr)
            if (e->d_name[0] != '.' && hasTxt(e->d_name))
                v.push_back(std::string("/book/") + e->d_name);
        closedir(d);
        std::sort(v.begin(), v.end());
        return v;
    }

    void tryLoadSelectedBook() {
        if (_bookPaths.empty()) { updateStatus("No .txt in /book/"); return; }
        s32 idx = _bookList ? _bookList->getSelectedIndex() : 0;
        if (idx < 0) idx = 0;
        if (idx < (s32)_bookPaths.size())
            loadAndShowBook(_bookPaths[idx].c_str());
    }

    // ── Woopsi overrides ───────────────────────────────────────────────────────

    void handleStylus(Gadget* gadget) override {
        const Stylus& s = Hardware::getStylus();
        if (s.isNewPress())
            handleClick(s.getX(), s.getY(), gadget);
        else if (s.isHeld()) {
            if (getClickedGadget())
                getClickedGadget()->drag(s.getX(), s.getY(), s.getVX(), s.getVY());
        } else if (getClickedGadget()) {
            getClickedGadget()->release(s.getX(), s.getY());
        }
    }

    void handleVBL() override {
        Woopsi::handleVBL();

        // Linger: exit browse mode ~0.5 s after releasing the scroll key.
        if (_browseLingerEnd > 0 && _vblCount >= _browseLingerEnd) {
            _browseLingerEnd = 0;
            exitBrowse();
        }

        // RSVP auto-advance is suspended during browse so the user can navigate
        // freely without the position jumping under their thumb.
        if (!_playing || _words.empty() || _browseMode) return;
        u32 frames = (u32)std::max(1, wordDurationMs() * 60 / 1000);
        if ((_vblCount - _wordStartVBL) >= frames) advanceWord();
    }

    void handleKeys() override {
        Woopsi::handleKeys();
        const uint16_t held = keysHeld();
        const uint16_t down = keysDown();
        const uint16_t up   = keysUp();

        if ((held & KEY_START) && (held & KEY_SELECT)) { stopModal(); return; }

        // A — Play/Pause or load
        if (down & KEY_A) {
            if (_words.empty()) tryLoadSelectedBook();
            else {
                _playing = !_playing;
                if (_playing) _wordStartVBL = _vblCount;
                updateStatus(_playing ? "Playing" : "Paused");
            }
        }
        // B — Rewind sentence
        if (down & KEY_B) { rewindSentence(); if (_playing) _wordStartVBL = _vblCount; }

        // L / R — WPM
        if (down & KEY_L) { _wpm = std::max(50,   _wpm - 25); updateWpmLabel(); }
        if (down & KEY_R) { _wpm = std::min(1000, _wpm + 25); updateWpmLabel(); }

        // Up / Down — jump ±10 words
        if (down & KEY_UP)   showWord(_currentWord + 10);
        if (down & KEY_DOWN) showWord(_currentWord - 10);

        // Left / Right — single step on press; continuous scroll when held.
        // After the initial hold delay (~333 ms = 20 frames), the word advances
        // every 5 frames (~83 ms) and browse mode activates.
        if (down & (KEY_LEFT | KEY_RIGHT)) {
            _lrHeldSince     = _vblCount;
            _lrLastRepeat    = _vblCount;
            _browseLingerEnd = 0;          // cancel any pending linger exit
            showWord(_currentWord + ((down & KEY_RIGHT) ? 1 : -1));
        } else if (held & (KEY_LEFT | KEY_RIGHT)) {
            const u32 heldFor  = _vblCount - _lrHeldSince;
            const u32 sinceRep = _vblCount - _lrLastRepeat;
            if (heldFor >= 20 && sinceRep >= 5) {
                _lrLastRepeat    = _vblCount;
                _browseLingerEnd = 0;      // keep linger clock reset while held
                showWord(_currentWord + ((held & KEY_RIGHT) ? 1 : -1));
                if (!_browseMode) enterBrowse();
            }
        }
        // Key released: start linger countdown (~0.5 s = 30 frames) so the
        // browse view stays visible briefly before snapping back to RSVP.
        if ((up & (KEY_LEFT | KEY_RIGHT)) && _browseMode && _browseLingerEnd == 0) {
            _browseLingerEnd = _vblCount + 30;
        }

        // Y — theme toggle
        if (down & KEY_Y) toggleTheme();

        // Start / Select
        if ((down & KEY_START)  && !(held & KEY_SELECT)) { saveState(); updateStatus("Saved!"); }
        if ((down & KEY_SELECT) && !(held & KEY_START))    tryLoadSelectedBook();
    }

public:
    void startup() override {

        // ── Top screen: borderless WordCanvas ─────────────────────────────────
        _topScreen = new AmigaScreen("", false, false);
        woopsiApplication->addGadget(_topScreen);
        _topScreen->flipToTopScreen();
        _topScreen->setBorderless(true);

        _wordCanvas = new WordCanvas(0, 0, 256, 192);
        _topScreen->addGadget(_wordCanvas);

        // ── Bottom screen ─────────────────────────────────────────────────────
        _bottomScreen = new AmigaScreen("Controls", false, false);
        woopsiApplication->addGadget(_bottomScreen);

        AmigaWindow* botWin = new AmigaWindow(0, 13, 256, 179, "Controls", false, false);
        _bottomScreen->addGadget(botWin);

        Rect bot; botWin->getClientRect(bot);
        s16 y = bot.y;

        // ── Row 1: book filename + theme button ───────────────────────────────
        _bookLabel = new Label(bot.x, y, bot.width - 52, 14, "No book loaded");
        _bookLabel->setTextAlignmentHoriz(Label::TEXT_ALIGNMENT_HORIZ_CENTRE);
        botWin->addGadget(_bookLabel);

        _themeBtn = new Button(bot.x + bot.width - 52, y, 52, 14, "Light");
        _themeBtn->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget&) { toggleTheme(); }
        ));
        botWin->addGadget(_themeBtn);
        y += 14;

        // ── Row 2: WPM +/- | Font size +/- ───────────────────────────────────
        const s16 btnW = 24, lblW1 = 62, lblW2 = 36, grpGap = 8;

        auto addWpmMinus = new Button(bot.x, y, btnW, 14, "-");
        addWpmMinus->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget&) { _wpm = std::max(50, _wpm - 25); updateWpmLabel(); }
        ));
        botWin->addGadget(addWpmMinus);

        _wpmLabel = new Label(bot.x + btnW, y, lblW1, 14, "WPM: 300");
        botWin->addGadget(_wpmLabel);

        auto addWpmPlus = new Button(bot.x + btnW + lblW1, y, btnW, 14, "+");
        addWpmPlus->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget&) { _wpm = std::min(1000, _wpm + 25); updateWpmLabel(); }
        ));
        botWin->addGadget(addWpmPlus);

        s16 szX = bot.x + btnW + lblW1 + btnW + grpGap;

        auto addSzMinus = new Button(szX, y, btnW, 14, "-");
        addSzMinus->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget&) {
                _wordCanvas->setMaxScale(_wordCanvas->getMaxScale() - 1);
                updateFontSizeLabel();
            }
        ));
        botWin->addGadget(addSzMinus);

        _fontSizeLabel = new Label(szX + btnW, y, lblW2, 14, "3x");
        botWin->addGadget(_fontSizeLabel);

        auto addSzPlus = new Button(szX + btnW + lblW2, y, btnW, 14, "+");
        addSzPlus->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget&) {
                _wordCanvas->setMaxScale(_wordCanvas->getMaxScale() + 1);
                updateFontSizeLabel();
            }
        ));
        botWin->addGadget(addSzPlus);
        y += 14;

        // ── Row 3: progress ───────────────────────────────────────────────────
        _progressLabel = new Label(bot.x, y, bot.width, 12, "");
        botWin->addGadget(_progressLabel);
        y += 12;

        // ── Row 4: status ─────────────────────────────────────────────────────
        _statusLabel = new Label(bot.x, y, bot.width, 12, "Select a book");
        _statusLabel->setTextAlignmentHoriz(Label::TEXT_ALIGNMENT_HORIZ_CENTRE);
        botWin->addGadget(_statusLabel);
        y += 12;

        // ── Row 5: book list ──────────────────────────────────────────────────
        _bookPaths = discoverBooks();
        _bookList  = new ListBox(bot.x, y, bot.width, 64);
        for (int i = 0; i < (int)_bookPaths.size(); i++) {
            const char* slash = strrchr(_bookPaths[i].c_str(), '/');
            const char* name  = slash ? slash + 1 : _bookPaths[i].c_str();
            _bookList->addOption(WoopsiString(name), (u32)i);
        }
        if (_bookPaths.empty())
            _bookList->addOption(WoopsiString("(no .txt files in /book/)"), 0);
        _bookList->setGadgetEventHandler(new GadgetCallback(
            [this](Gadget& g) {
                s32 idx = static_cast<ListBox&>(g).getSelectedIndex();
                if (idx >= 0 && idx < (s32)_bookPaths.size())
                    loadAndShowBook(_bookPaths[idx].c_str());
            }
        ));
        botWin->addGadget(_bookList);
        y += 66;

        // ── Hints ─────────────────────────────────────────────────────────────
        auto hint = [&](const char* t) {
            botWin->addGadget(new Label(bot.x, y, bot.width, 12, t)); y += 12;
        };
        hint("L/R:WPM  A:Play  B:Rewind  Sel:Load");
        hint("Dpad:Navigate(hold=browse)  Y:Theme");

        // ── Restore last session ──────────────────────────────────────────────
        if (loadSavedState()) {
            _wpm = std::max(50, std::min(1000, _wpm));
            updateWpmLabel();
            int savedWord = _currentWord;
            if (!_currentBookPath.empty() && loadBook(_currentBookPath.c_str())) {
                const char* slash = strrchr(_currentBookPath.c_str(), '/');
                const char* name  = slash ? slash + 1 : _currentBookPath.c_str();
                char buf[64]; snprintf(buf, sizeof(buf), "%.60s", name);
                _bookLabel->setText(WoopsiString(buf));
                showWord(std::min(savedWord, (int)_words.size() - 1));
                updateStatus("A to continue");
            } else {
                updateStatus("A to load book");
            }
        } else {
            updateStatus(_bookPaths.empty() ? "Put .txt in /book/" : "Tap book then A");
        }
    }
};

int main(int argc, char** argv) {
    fatInitDefault();
    RSVPReaderApp app;
    return app.main(argc, argv);
}
