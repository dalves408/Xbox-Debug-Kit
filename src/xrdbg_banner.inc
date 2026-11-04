/* In-game banner implementation, included by xrdbg.c. */
static SRWLOCK xrdbg_banner_lock = SRWLOCK_INIT;
static HWND xrdbg_banner_game, xrdbg_banner_window;
static struct banner_args *xrdbg_banner_pending;

static void xrdbg_banner_layout(void)
{
    RECT r;
    int cw,ch,margin,w,h,ideal,dpi=96;
    if(!xrdbg_banner_window||!GetClientRect(xrdbg_banner_game,&r))return;
    cw=r.right;ch=r.bottom;
    if(cw<=0||ch<=0){ShowWindow(xrdbg_banner_window,SW_HIDE);return;}
    {
        typedef UINT (WINAPI *dpi_fn)(HWND);
        HMODULE u=GetModuleHandleA("user32.dll");
        dpi_fn get_dpi=u?(dpi_fn)(void *)GetProcAddress(u,"GetDpiForWindow"):NULL;
        if(get_dpi)dpi=(int)get_dpi(xrdbg_banner_game);
    }
    margin=MulDiv(12,dpi,96);if(margin>cw/16)margin=cw/16;if(margin>ch/16)margin=ch/16;
    ideal=MulDiv(680,dpi,96);
    w=cw-2*margin;if(w>ideal)w=ideal;if(w<1)w=1;
    h=MulDiv(215,w,680);if(h<1)h=1;
    if(h>ch-2*margin){h=ch-2*margin;if(h<1)h=1;w=MulDiv(680,h,215);if(w<1)w=1;}
    SetWindowPos(xrdbg_banner_window,HWND_TOP,(cw-w)/2,margin,w,h,
                 SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(xrdbg_banner_window,NULL,FALSE);
}

static LRESULT CALLBACK xrdbg_banner_proc(HWND h,UINT m,WPARAM w,LPARAM l)
{
    struct banner_args *ba=(struct banner_args *)GetWindowLongPtrA(h,GWLP_USERDATA);
    if(m==WM_NCCREATE){
        ba=(struct banner_args *)((CREATESTRUCTA *)l)->lpCreateParams;
        SetWindowLongPtrA(h,GWLP_USERDATA,(LONG_PTR)ba);
    }
    if(m==WM_TIMER){DestroyWindow(h);return 0;}
    if(m==WM_NCHITTEST)return HTTRANSPARENT;
    if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(m==WM_ERASEBKGND)return 1;
    if(m==WM_PAINT&&ba){
        PAINTSTRUCT ps;RECT cr,r;HDC dc=BeginPaint(h,&ps),mem;
        HBITMAP bitmap,old_bitmap;HFONT f1,f2,f3,old_font;HBRUSH brush;
        int width,height,scale;
        GetClientRect(h,&cr);width=cr.right;height=cr.bottom;
        mem=CreateCompatibleDC(dc);bitmap=CreateCompatibleBitmap(dc,width>0?width:1,height>0?height:1);
        if(mem&&bitmap){
            old_bitmap=(HBITMAP)SelectObject(mem,bitmap);
            brush=CreateSolidBrush(ba->accent);FillRect(mem,&cr,brush);DeleteObject(brush);
            r=cr;scale=MulDiv(4,width,680);if(scale<1)scale=1;InflateRect(&r,-scale,-scale);
            brush=CreateSolidBrush(RGB(14,22,40));FillRect(mem,&r,brush);DeleteObject(brush);
            SetBkMode(mem,TRANSPARENT);
            f1=CreateFontA(-MulDiv(46,width,680),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,"Segoe UI");
            f2=CreateFontA(-MulDiv(26,width,680),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,"Segoe UI");
            f3=CreateFontA(-MulDiv(17,width,680),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,"Segoe UI");
            old_font=(HFONT)SelectObject(mem,f1);SetTextColor(mem,ba->accent);
            SetRect(&r,0,MulDiv(18,height,215),width,MulDiv(76,height,215));
            DrawTextA(mem,ba->title,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SelectObject(mem,f2);SetTextColor(mem,RGB(255,255,255));
            SetRect(&r,0,MulDiv(80,height,215),width,MulDiv(116,height,215));
            DrawTextA(mem,ba->l2,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
            SelectObject(mem,f3);SetTextColor(mem,RGB(200,210,225));
            SetRect(&r,MulDiv(12,width,680),MulDiv(122,height,215),width-MulDiv(12,width,680),MulDiv(205,height,215));
            DrawTextA(mem,ba->l3,-1,&r,DT_CENTER|DT_WORDBREAK|DT_END_ELLIPSIS);
            SelectObject(mem,old_font);DeleteObject(f1);DeleteObject(f2);DeleteObject(f3);
            BitBlt(dc,0,0,width,height,mem,0,0,SRCCOPY);
            SelectObject(mem,old_bitmap);
        }
        if(bitmap)DeleteObject(bitmap);if(mem)DeleteDC(mem);EndPaint(h,&ps);return 0;
    }
    if(m==WM_NCDESTROY){
        int owned=xrdbg_banner_window==h;
        if(owned)xrdbg_banner_window=NULL;
        SetWindowLongPtrA(h,GWLP_USERDATA,0);
        if(owned)free(ba); /* CreateWindow failure is still owned by its caller. */
    }
    return DefWindowProcA(h,m,w,l);
}

static void xrdbg_banner_display(HWND game,struct banner_args *ba)
{
    WNDCLASSA wc;
    if(!ba)return;
    if(!IsWindow(game)){free(ba);return;}
    if(xrdbg_banner_window)DestroyWindow(xrdbg_banner_window);
    memset(&wc,0,sizeof wc);wc.lpfnWndProc=xrdbg_banner_proc;
    wc.hInstance=GetModuleHandleA(NULL);wc.lpszClassName="XrdbgChildBanner";
    RegisterClassA(&wc);
    xrdbg_banner_window=CreateWindowExA(WS_EX_NOACTIVATE|WS_EX_NOPARENTNOTIFY|WS_EX_TRANSPARENT,
        wc.lpszClassName,"",WS_CHILD,0,0,1,1,game,NULL,wc.hInstance,ba);
    if(!xrdbg_banner_window){free(ba);return;}
    xrdbg_banner_layout();UpdateWindow(xrdbg_banner_window);
    SetTimer(xrdbg_banner_window,1,ba->ms,NULL);
    fprintf(stderr,"[DEBUG] banner \"%s\" shown for %lu ms inside game window (%s)\n",
            ba->title,(unsigned long)ba->ms,ba->l2);
}


/* Attach and window-message hooks run on the parent window's thread. Cross-thread
 * requests transfer no pointers through the message queue: only the latest banner
 * is retained, and destroying the parent frees it even if the message never runs. */
static UINT xrdbg_banner_message_id;

void xrdbg_attach_window(HWND game)
{
    struct banner_args *pending;
    if (!s_cfg.banner_in_game || !IsWindow(game) ||
        GetWindowThreadProcessId(game, NULL) != GetCurrentThreadId()) return;
    if (xrdbg_banner_game && xrdbg_banner_game != game) return;
    xrdbg_banner_message_id = RegisterWindowMessageA("xrdbg.banner.request.v1");
    if (!xrdbg_banner_message_id) return;
    SetWindowLongPtrA(game, GWL_STYLE, GetWindowLongPtrA(game, GWL_STYLE) | WS_CLIPCHILDREN);
    AcquireSRWLockExclusive(&xrdbg_banner_lock);
    xrdbg_banner_game = game;
    pending = xrdbg_banner_pending;
    xrdbg_banner_pending = NULL;
    ReleaseSRWLockExclusive(&xrdbg_banner_lock);
    if (pending) xrdbg_banner_display(game, pending);
    else xrdbg_banner_layout();
}

int xrdbg_window_message(HWND game, UINT m, WPARAM w, LPARAM l)
{
    (void)w; (void)l;
    if (!s_cfg.banner_in_game || game != xrdbg_banner_game) return 0;
    if (xrdbg_banner_message_id && m == xrdbg_banner_message_id) {
        struct banner_args *pending;
        AcquireSRWLockExclusive(&xrdbg_banner_lock);
        pending = xrdbg_banner_pending;
        xrdbg_banner_pending = NULL;
        ReleaseSRWLockExclusive(&xrdbg_banner_lock);
        xrdbg_banner_display(game, pending);
        return 1;
    }
    if (m == WM_SIZE || m == WM_DPICHANGED) xrdbg_banner_layout();
    if (m == WM_NCDESTROY) {
        AcquireSRWLockExclusive(&xrdbg_banner_lock);
        xrdbg_banner_game = NULL;
        free(xrdbg_banner_pending);
        xrdbg_banner_pending = NULL;
        ReleaseSRWLockExclusive(&xrdbg_banner_lock);
    }
    return 0;
}

static void show_banner(const char *title, const char *l2, const char *l3, DWORD ms, COLORREF accent)
{
    HWND game;
    struct banner_args *ba;
    if (!s_cfg.banner_in_game) {
        desktop_show_banner(title, l2, l3, ms, accent);
        return;
    }
    ba = (struct banner_args *)calloc(1, sizeof *ba);
    if (!ba) return;
    snprintf(ba->title, sizeof ba->title, "%s", title);
    snprintf(ba->l2, sizeof ba->l2, "%s", l2);
    snprintf(ba->l3, sizeof ba->l3, "%s", l3);
    ba->ms = ms; ba->accent = accent;
    AcquireSRWLockExclusive(&xrdbg_banner_lock);
    game = xrdbg_banner_game;
    free(xrdbg_banner_pending);
    xrdbg_banner_pending = ba;
    if (game) PostMessageA(game, xrdbg_banner_message_id, 0, 0);
    ReleaseSRWLockExclusive(&xrdbg_banner_lock);
    if (game && GetWindowThreadProcessId(game, NULL) == GetCurrentThreadId())
        xrdbg_window_message(game, xrdbg_banner_message_id, 0, 0);
}
