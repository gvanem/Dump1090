/*
  Windows clipboard code shared by WinCon,  WinGUI,  Windows
  builds for VT and (at some point) SDL1.
 */

/*

### Description

   PDC_getclipboard() gets the textual contents of the system's
   clipboard. This function returns the contents of the clipboard in the
   contents argument. It is the responsibility of the caller to free the
   memory returned, via PDC_freeclipboard(). The length of the clipboard
   contents is returned in the length argument.

   PDC_setclipboard() copies the supplied text into the system's
   clipboard, emptying the clipboard prior to the copy.

   PDC_clearclipboard() clears the internal clipboard.

### Return Values

    PDC_CLIP_SUCCESS        the call was successful
    PDC_CLIP_MEMORY_ERROR   unable to allocate sufficient memory for
                            the clipboard contents
    PDC_CLIP_EMPTY          the clipboard contains no text
    PDC_CLIP_ACCESS_ERROR   no clipboard support

 */

#define PDC_TEXT CF_UNICODETEXT

int PDC_getclipboard(char **contents, long *length)
{
    HANDLE handle;
    long len;

    if (!OpenClipboard(NULL))
        return PDC_CLIP_ACCESS_ERROR;

    if ((handle = GetClipboardData(PDC_TEXT)) == NULL)
    {
        CloseClipboard();
        return PDC_CLIP_EMPTY;
    }

    len = (long)wcslen((wchar_t *)handle) * 3;
    *contents = (char *)GlobalAlloc(GMEM_FIXED, len + 1);

    if (!*contents)
    {
        CloseClipboard();
        return PDC_CLIP_MEMORY_ERROR;
    }

    len = (long)PDC_wcstombs((char *)*contents, (wchar_t *)handle, len);
    *length = len;
    CloseClipboard();

    return PDC_CLIP_SUCCESS;
}

int PDC_setclipboard(const char *contents, long length)
{
    HGLOBAL ptr1;
    LPTSTR ptr2;

    if (!OpenClipboard(NULL))
        return PDC_CLIP_ACCESS_ERROR;

    ptr1 = GlobalAlloc(GMEM_MOVEABLE|GMEM_DDESHARE,
        (length + 1) * sizeof(TCHAR));

    if (!ptr1)
        return PDC_CLIP_MEMORY_ERROR;

    ptr2 = GlobalLock(ptr1);

    PDC_mbstowcs((wchar_t *)ptr2, contents, length);
    GlobalUnlock(ptr1);
    EmptyClipboard();

    if (!SetClipboardData(PDC_TEXT, ptr1))
    {
        GlobalFree(ptr1);
        return PDC_CLIP_ACCESS_ERROR;
    }

    CloseClipboard();
    GlobalFree(ptr1);

    return PDC_CLIP_SUCCESS;
}

int PDC_freeclipboard(char *contents)
{
    GlobalFree(contents);
    return PDC_CLIP_SUCCESS;
}

int PDC_clearclipboard(void)
{
    int rval = PDC_CLIP_ACCESS_ERROR;

    if (OpenClipboard(NULL))
    {
        if( EmptyClipboard())
            rval = PDC_CLIP_SUCCESS;
        CloseClipboard();
    }
    return rval;
}
