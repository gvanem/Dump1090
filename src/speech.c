/**\file    speech.c
 * \ingroup Misc
 * \brief   Simple SAPI5 speech-interface.
 *
 * SAPI 5.4 overview:
 *   https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee125077(v=vs.85)
 */
#define CINTERFACE
#define COBJMACROS

#include "speech.h"
#include "smartlist.h"

#include <sapi.h>
#include <sperror.h>

#undef TRACE

#if defined(TEST)
  #define TRACE(level, fmt, ...) do {                       \
            if (g_data.trace_level >= level)                \
               printf ("%s(%u): " fmt,                      \
                       __FILE__, __LINE__, ## __VA_ARGS__); \
          } while (0)

#else
  /*
   * Only write important stuff to the .log-file.
   */
  #define TRACE(level, fmt, ...)  do {         \
          if (level <= 1)                      \
             LOG_FILEONLY ("%s(%u): " fmt,     \
                           __FILE__, __LINE__, \
                           ## __VA_ARGS__);    \
          } while (0)
#endif

typedef struct speak_queue {
        wchar_t       *wstr;
        double         start_t;
        bool           finished;
        DWORD          flags, id;
        SPVOICESTATUS  status, old_status;
      } speak_queue;

typedef struct speech_data {
        smartlist_t      *speak_queue;
        ISpVoice         *voice;
        int               voice_n;
        int               trace_level;
        HANDLE            thread_hnd;
        DWORD             start_id;
        HRESULT           hr_err;
        CRITICAL_SECTION  crit;
        bool              CoInitializeEx_done;
        bool              quit;
      } speech_data;

static speech_data  g_data;

static void         speak_queue_free (void);
static DWORD WINAPI speak_thread (void *arg);
static const char  *hr_strerror (HRESULT hr);
static const char  *sp_running_state (SPRUNSTATE state);

#define CALL(obj, func, ...) do {                                          \
          hr = (*obj -> lpVtbl -> func) (obj, __VA_ARGS__);                \
          if (!SUCCEEDED(hr))                                              \
          {                                                                \
            g_data.hr_err = hr;                                            \
            TRACE (1, #obj "::" #func "() failed: %s\n", hr_strerror(hr)); \
            return (false);                                                \
          }                                                                \
        } while (0)

#define CALL0(obj, func) do {                                              \
          hr = (*obj -> lpVtbl -> func) (obj);                             \
          if (!SUCCEEDED(hr))                                              \
          {                                                                \
            g_data.hr_err = hr;                                            \
            TRACE (1, #obj "::" #func "() failed: %s\n", hr_strerror(hr)); \
            return (false);                                                \
          }                                                                \
        } while (0)

/*
 * Avoid linking with 'SAPI.lib' and define these here.
 */
#undef  DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
        const GUID DECLSPEC_SELECTANY name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }

DEFINE_GUID (IID_ISpObjectTokenCategory,  0x2D3D3845, 0x39AF, 0x4850, 0xBB, 0xF9, 0x40, 0xB4, 0x97, 0x80, 0x01, 0x1D);
DEFINE_GUID (IID_ISpVoice,                0x6C44DF74, 0x72B9, 0x4992, 0xA1, 0xEC, 0xEF, 0x99, 0x6e, 0x04, 0x22, 0xD4);
DEFINE_GUID (CLSID_SpObjectTokenCategory, 0xA910187F, 0x0C7A, 0x45AC, 0x92, 0xCC, 0x59, 0xED, 0xaf, 0xB7, 0x7B, 0x53);
DEFINE_GUID (CLSID_SpVoice,               0x96749377, 0x3391, 0x11D2, 0x9E, 0xE3, 0x00, 0xC0, 0x4f, 0x79, 0x73, 0x96);

#if 0
/**
 * A rewrite of SpGetDescription() in sphelper.h which is for C++ only
 */
static HRESULT SpGetDescription (ISpObjectToken * pObjToken, _Outptr_ PWSTR *ppszDescription)
{
  LANGID Language = SpGetUserDefaultUILanguage();
}
#endif

/**
 * Enumerate the available voices
 * Ref:
 *   https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms719807(v=vs.85)
 */
static bool enumerate_voices (int *voice_p)
{
  HRESULT hr;
  DWORD   count = 0;
  int     num = 0;
  bool    rc = false;
  ISpObjectTokenCategory *category  = NULL;
  IEnumSpObjectTokens    *enum_tok  = NULL;
  ISpObjectToken         *voice_tok = NULL;
  ISpDataKey             *data_attr = NULL;

  hr = CoCreateInstance (&CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                         &IID_ISpObjectTokenCategory, (void**)&category);
  if (!SUCCEEDED(hr))
  {
    TRACE (0, "CoCreateInstance() failed: %s\n", hr_strerror(hr));
    g_data.hr_err = hr;
    goto failed;
  }

  CALL (category, SetId, SPCAT_VOICES, FALSE);
  CALL (category, EnumTokens, NULL, NULL, &enum_tok);
  CALL (enum_tok, GetCount, &count);

  while (SUCCEEDED(hr) && count-- > 0)
  {
    wchar_t *w_id   = NULL;
    wchar_t *w_lang = NULL;
    wchar_t *w_name = NULL;
    wchar_t *w_lang_code;
    wchar_t *w_reg_code;
    int      locale_chars;
    int      region_chars;
    LCID     locale;

    CALL (enum_tok, Next, 1, &voice_tok, NULL);
    CALL (voice_tok, OpenKey, SPTOKENKEY_ATTRIBUTES, &data_attr);
    CALL (voice_tok, GetId, &w_id);
    CALL (data_attr, GetStringValue, L"Language", &w_lang);
//  CALL (data_attr, GetStringValue, NULL, &w_name);

    (void) w_name;

//  SpGetDescription ((*voice_tok->lpVtbl->Get)(voice_tok), &description);

    locale = wcstol (w_lang, NULL, 16);
    locale_chars = GetLocaleInfoW (locale, LOCALE_SISO639LANGNAME, NULL, 0);
    region_chars = GetLocaleInfoW (locale, LOCALE_SISO3166CTRYNAME, NULL, 0);
    w_lang_code  = alloca (sizeof(wchar_t) * locale_chars);
    w_reg_code   = alloca (sizeof(wchar_t) * region_chars);

    GetLocaleInfoW (locale, LOCALE_SISO639LANGNAME, w_lang_code, locale_chars);
    GetLocaleInfoW (locale, LOCALE_SISO3166CTRYNAME, w_reg_code, region_chars);

    TRACE (2, "w_id: '%S', w_lang_code: '%S' (%lu)\n",
           w_id, w_lang_code, locale);

    rc = true;

   /**
    * \todo Get 'Gender' and 'Name' from Registry.
    *
    * The output of:
    *   reg.exe query "HKLM\SOFTWARE\Microsoft\Speech\Voices\Tokens"  /s:
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-GB_HAZEL_11.0
    *       (Default)     REG_SZ          Microsoft Hazel Desktop - English (Great Britain)
    *       809           REG_SZ          Microsoft Hazel Desktop - English (Great Britain)
    *       CLSID         REG_SZ          {179F3D56-1B0B-42B2-A962-59B7EF59FE1B}
    *       LangDataPath  REG_EXPAND_SZ   %windir%\Speech_OneCore\Engines\TTS\en-GB\MSTTSLocEnGB.dat
    *       VoicePath     REG_EXPAND_SZ   %windir%\Speech_OneCore\Engines\TTS\en-GB\M2057Hazel
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-GB_HAZEL_11.0\Attributes
    *       Age           REG_SZ  Adult
    *       Gender        REG_SZ  Female
    *       Language      REG_SZ  809
    *       Name          REG_SZ  Microsoft Hazel Desktop
    *       SpLexicon     REG_SZ  {0655E396-25D0-11D3-9C26-00C04F8EF87C}
    *       Vendor        REG_SZ  Microsoft
    *       Version       REG_SZ  11.0
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-US_DAVID_11.0
    *       (Default)     REG_SZ         Microsoft David Desktop - English (United States)
    *       409           REG_SZ         Microsoft David Desktop - English (United States)
    *       CLSID         REG_SZ         {179F3D56-1B0B-42B2-A962-59B7EF59FE1B}
    *       LangDataPath  REG_EXPAND_SZ  %windir%\Speech_OneCore\Engines\TTS\en-US\MSTTSLocEnUS.dat
    *       VoicePath     REG_EXPAND_SZ  %windir%\Speech_OneCore\Engines\TTS\en-US\M1033David
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-US_DAVID_11.0\Attributes
    *       Age           REG_SZ  Adult
    *       Gender        REG_SZ  Male
    *       Language      REG_SZ  409
    *       Name          REG_SZ  Microsoft David Desktop
    *       SpLexicon     REG_SZ  {0655E396-25D0-11D3-9C26-00C04F8EF87C}
    *       Vendor        REG_SZ  Microsoft
    *       Version       REG_SZ  11.0
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-US_ZIRA_11.0
    *       (Default)     REG_SZ         Microsoft Zira Desktop - English (United States)
    *       409           REG_SZ         Microsoft Zira Desktop - English (United States)
    *       CLSID         REG_SZ         {C64501F6-E6E6-451f-A150-25D0839BC510}
    *       LangDataPath  REG_EXPAND_SZ  %SystemRoot%\Speech\Engines\TTS\en-US\MSTTSLocEnUS.dat
    *       VoicePath     REG_EXPAND_SZ  %SystemRoot%\Speech\Engines\TTS\en-US\M1033ZIR
    *
    *   HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Speech\Voices\Tokens\TTS_MS_EN-US_ZIRA_11.0\Attributes
    *       Age           REG_SZ  Adult
    *       Gender        REG_SZ  Female
    *       Language      REG_SZ  409
    *       Name          REG_SZ  Microsoft Zira Desktop
    *       Vendor        REG_SZ  Microsoft
    *       Version       REG_SZ  11.0
    */

    CALL0 (voice_tok, Release);
  }

  *voice_p = num;

failed:
  if (enum_tok)
     CALL0 (enum_tok, Release);

  if (category)
     CALL0 (category, Release);

  return (rc);
}

bool speak_init (int voice, int volume)
{
  HRESULT hr;

  if (volume < 0 || volume > 100)
  {
    TRACE (0, "'volume' must be in range 0 - 100\n");
    return (false);
  }

  g_data.speak_queue = smartlist_new();
  if (!g_data.speak_queue)
  {
    TRACE (0, "No memory for 'speak_queue'\n");
    return (false);
  }

  hr = CoInitializeEx (NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
  if (!SUCCEEDED(hr))
  {
    TRACE (0, "CoInitializeEx() failed: %s\n", hr_strerror(hr));
    g_data.hr_err = hr;
    return (false);
  }

  g_data.CoInitializeEx_done = true;

  hr = CoCreateInstance (&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void**)&g_data.voice);
  if (!SUCCEEDED(hr))
  {
    TRACE (0, "CoCreateInstance() failed: %s\n", hr_strerror(hr));
    g_data.hr_err = hr;
    return (false);
  }

#if 1
  g_data.voice_n = voice;

  if (enumerate_voices(&g_data.voice_n))
     ; // (*g_data.voice->lpVtbl->SetVoice) (g_data.voice, g_data.voice_n);
#endif

  CALL (g_data.voice, SetVolume, volume);

  if (g_data.thread_hnd)
  {
    TRACE (0, "Already have 'thread_hnd'. Call 'speak_exit()' first\n");
    return (false);
  }
  InitializeCriticalSection (&g_data.crit);

  g_data.thread_hnd = CreateThread (NULL, 0, speak_thread, (void*)&g_data.speak_queue, 0, NULL);
  if (!g_data.thread_hnd)
  {
    TRACE (0, "CreateThread() failed: %s\n", win_strerror(GetLastError()));
    return (false);
  }

  g_data.quit = false;
  return (true);
}

void speak_exit (void)
{
  DeleteCriticalSection (&g_data.crit);

  if (g_data.voice)
  {
    (*g_data.voice->lpVtbl->Speak) (g_data.voice, NULL, SPF_PURGEBEFORESPEAK, NULL);
    (*g_data.voice->lpVtbl->Release) (g_data.voice);
  }

  if (g_data.CoInitializeEx_done)
  {
    TRACE (2, "Calling 'CoUninitialize()'\n");
    CoUninitialize();
  }

  if (g_data.thread_hnd)
  {
    TerminateThread (g_data.thread_hnd, 0);
    CloseHandle (g_data.thread_hnd);
  }

  speak_queue_free();
  memset (&g_data, '\0', sizeof(g_data));
}

/*
 * Add an ASCII-string to the global queue.
 */
static bool speak_queue_add (const char *str)
{
  size_t       len = strlen (str);
  speak_queue *sq  = calloc (sizeof(*sq) + sizeof(*sq->wstr) * (len + 1), 1);

  if (!sq)
     return (false);

  EnterCriticalSection (&g_data.crit);

  sq->flags = (SPF_ASYNC | SPF_IS_XML);
  sq->id    = g_data.start_id++;
  sq->wstr  = (wchar_t*) (sq + 1);

  mbstowcs (sq->wstr, str, len);
  sq->wstr [len] = L'\0';

  smartlist_add (g_data.speak_queue, sq);

  LeaveCriticalSection (&g_data.crit);
  return (true);
}

/*
 * Free the global queue.
 */
static void speak_queue_free (void)
{
  if (g_data.speak_queue)
     smartlist_wipe (g_data.speak_queue, free);
  g_data.speak_queue = NULL;
}

static bool speak_finished (speak_queue *sq)
{
  bool    changed;
  HRESULT hr;

  if (!sq)
     return (false);

  memset (&sq->status, '\0', sizeof(sq->status));
  CALL (g_data.voice, GetStatus, &sq->status, NULL);

  changed = (sq->status.dwRunningState != sq->old_status.dwRunningState ||
             sq->status.ulInputWordPos != sq->old_status.ulInputWordPos ||
             sq->status.PhonemeId      != sq->old_status.PhonemeId      ||
             sq->status.VisemeId       != sq->old_status.VisemeId);
  memcpy (&sq->old_status, &sq->status, sizeof(sq->old_status));
  if (!changed)
     return (false);

  TRACE (2, "%lu: %10.3lf ms, dwRunningState: %s, InputWordPos: %lu, PhonemeId: %d, VisemeId: %d\n",
         sq->id,
         (get_usec_now() - sq->start_t) / 1E3,
         sp_running_state(sq->status.dwRunningState),
         sq->status.ulInputWordPos,
         sq->status.PhonemeId,
         sq->status.VisemeId);
  return (sq->status.dwRunningState == SPRS_DONE);
}

static int speak_queue_len (void)
{
  return smartlist_len (g_data.speak_queue);
}

static int speak_queue_unfinished (void)
{
  const speak_queue *sq;
  int   num = 0;
  int   i, max = smartlist_len (g_data.speak_queue);

  for (i = 0; i < max; i++)
  {
    sq = smartlist_get (g_data.speak_queue, i);
    if (!sq->finished)
       num++;
  }
  return (num);
}

static bool speak_poll (void)
{
  int sq_sz = speak_queue_unfinished();

  if (sq_sz == 0)
  {
    if (g_data.quit)
         TRACE (0, "Sentences interrupted\n");
    else if (g_data.hr_err)
         TRACE (0, "A sentence failed: %s\n", hr_strerror(g_data.hr_err));
    else TRACE (0, "All sentences completed\n");
    g_data.quit = true;
  }
  return (!g_data.quit || sq_sz > 0);
}

static DWORD WINAPI speak_thread (void *arg)
{
  int           sq_active_idx = -1;
  speak_queue  *sq_active = NULL;
  smartlist_t **queue = (smartlist_t**) arg;

  while (!g_data.quit)
  {
    speak_queue *sq;
    HRESULT      hr;

    EnterCriticalSection (&g_data.crit);

    /* Find first unfinished element for calling `g_data.voice::Speak()`.
     * All others must wait it's turn.
     */
    int i, max = smartlist_len (*queue);

    for (i = 0; i < max; i++)
    {
      sq = smartlist_get (*queue, i);
      if (!sq_active && !sq->finished)
      {
        sq_active_idx = i;
        sq_active = sq;
        sq_active->start_t = get_usec_now();
        CALL (g_data.voice, Speak, sq_active->wstr, sq_active->flags, NULL);
        TRACE (2, "g_data.voice::Speak(): %s\n", hr_strerror(hr));
        break;
      }
    }
    LeaveCriticalSection (&g_data.crit);
    Sleep (100);

    if (speak_finished(sq_active))
    {
      sq_active->finished = true;
#if defined(TEST)
      TRACE (1, "sq_active->id: %lu, SPRS_DONE, unfinished: %d\n",
             sq_active->id, speak_queue_unfinished());
#endif

      smartlist_del (*queue, sq_active_idx);
      sq_active = NULL;
    }
  }
  ExitThread (0);
  return (0);
}

bool speak_string (const char *fmt, ...)
{
  char    buf [10000];
  va_list args;

  va_start (args, fmt);
  vsnprintf (buf, sizeof(buf), fmt, args);
  va_end (args);
  return speak_queue_add (buf);
}

#define ADD_VALUE(v)  { (DWORD)v, #v }

#if !defined(__MINGW64__) || defined(HAVE_SAPI_ERRORS)

static const search_list hr_errors[] = {
       ADD_VALUE (S_OK),
       ADD_VALUE (S_FALSE),
       ADD_VALUE (RPC_E_CHANGED_MODE),
       ADD_VALUE (CO_E_NOTINITIALIZED),
       ADD_VALUE (E_INVALIDARG),
       ADD_VALUE (E_OUTOFMEMORY),
       ADD_VALUE (E_UNEXPECTED),
       ADD_VALUE (SPERR_UNINITIALIZED),
       ADD_VALUE (SPERR_ALREADY_INITIALIZED),
       ADD_VALUE (SPERR_UNSUPPORTED_FORMAT),
       ADD_VALUE (SPERR_INVALID_FLAGS),
       ADD_VALUE (SPERR_DEVICE_BUSY),
       ADD_VALUE (SPERR_DEVICE_NOT_SUPPORTED),
       ADD_VALUE (SPERR_DEVICE_NOT_ENABLED),
       ADD_VALUE (SPERR_NO_DRIVER),
       ADD_VALUE (SPERR_FILE_MUST_BE_UNICODE),
       ADD_VALUE (SPERR_INVALID_PHRASE_ID),
       ADD_VALUE (SPERR_BUFFER_TOO_SMALL),
       ADD_VALUE (SPERR_FORMAT_NOT_SPECIFIED),
       ADD_VALUE (SPERR_AUDIO_STOPPED),
       ADD_VALUE (SPERR_RULE_NOT_FOUND),
       ADD_VALUE (SPERR_TTS_ENGINE_EXCEPTION),
       ADD_VALUE (SPERR_TTS_NLP_EXCEPTION),
       ADD_VALUE (SPERR_ENGINE_BUSY),
       ADD_VALUE (SPERR_CANT_CREATE),
       ADD_VALUE (SPERR_NOT_IN_LEX),
       ADD_VALUE (SPERR_LEX_VERY_OUT_OF_SYNC),
       ADD_VALUE (SPERR_UNDEFINED_FORWARD_RULE_REF),
       ADD_VALUE (SPERR_EMPTY_RULE),
       ADD_VALUE (SPERR_GRAMMAR_COMPILER_INTERNAL_ERROR),
       ADD_VALUE (SPERR_RULE_NOT_DYNAMIC),
       ADD_VALUE (SPERR_DUPLICATE_RULE_NAME),
       ADD_VALUE (SPERR_DUPLICATE_RESOURCE_NAME),
       ADD_VALUE (SPERR_TOO_MANY_GRAMMARS),
       ADD_VALUE (SPERR_CIRCULAR_REFERENCE),
       ADD_VALUE (SPERR_INVALID_IMPORT),
       ADD_VALUE (SPERR_INVALID_WAV_FILE),
       ADD_VALUE (SPERR_ALL_WORDS_OPTIONAL),
       ADD_VALUE (SPERR_INSTANCE_CHANGE_INVALID),
       ADD_VALUE (SPERR_RULE_NAME_ID_CONFLICT),
       ADD_VALUE (SPERR_NO_RULES),
       ADD_VALUE (SPERR_CIRCULAR_RULE_REF),
       ADD_VALUE (SPERR_INVALID_HANDLE),
       ADD_VALUE (SPERR_REMOTE_CALL_TIMED_OUT),
       ADD_VALUE (SPERR_AUDIO_BUFFER_OVERFLOW),
       ADD_VALUE (SPERR_NO_AUDIO_DATA),
       ADD_VALUE (SPERR_DEAD_ALTERNATE),
       ADD_VALUE (SPERR_HIGH_LOW_CONFIDENCE),
       ADD_VALUE (SPERR_INVALID_FORMAT_STRING),
       ADD_VALUE (SPERR_APPLEX_READ_ONLY),
       ADD_VALUE (SPERR_NO_TERMINATING_RULE_PATH),
       ADD_VALUE (SPERR_STREAM_CLOSED),
       ADD_VALUE (SPERR_NO_MORE_ITEMS),
       ADD_VALUE (SPERR_NOT_FOUND),
       ADD_VALUE (SPERR_INVALID_AUDIO_STATE),
       ADD_VALUE (SPERR_GENERIC_MMSYS_ERROR),
       ADD_VALUE (SPERR_MARSHALER_EXCEPTION),
       ADD_VALUE (SPERR_NOT_DYNAMIC_GRAMMAR),
       ADD_VALUE (SPERR_AMBIGUOUS_PROPERTY),
       ADD_VALUE (SPERR_INVALID_REGISTRY_KEY),
       ADD_VALUE (SPERR_INVALID_TOKEN_ID),
       ADD_VALUE (SPERR_XML_BAD_SYNTAX),
       ADD_VALUE (SPERR_XML_RESOURCE_NOT_FOUND),
       ADD_VALUE (SPERR_TOKEN_IN_USE),
       ADD_VALUE (SPERR_TOKEN_DELETED),
       ADD_VALUE (SPERR_MULTI_LINGUAL_NOT_SUPPORTED),
       ADD_VALUE (SPERR_EXPORT_DYNAMIC_RULE),
       ADD_VALUE (SPERR_STGF_ERROR),
       ADD_VALUE (SPERR_WORDFORMAT_ERROR),
       ADD_VALUE (SPERR_STREAM_NOT_ACTIVE),
       ADD_VALUE (SPERR_ENGINE_RESPONSE_INVALID),
       ADD_VALUE (SPERR_SR_ENGINE_EXCEPTION),
       ADD_VALUE (SPERR_STREAM_POS_INVALID),
       ADD_VALUE (SPERR_REMOTE_CALL_ON_WRONG_THREAD),
       ADD_VALUE (SPERR_REMOTE_PROCESS_TERMINATED),
       ADD_VALUE (SPERR_REMOTE_PROCESS_ALREADY_RUNNING),
       ADD_VALUE (SPERR_LANGID_MISMATCH),
       ADD_VALUE (SPERR_NOT_TOPLEVEL_RULE),
       ADD_VALUE (SPERR_LEX_REQUIRES_COOKIE),
       ADD_VALUE (SPERR_UNSUPPORTED_LANG),
       ADD_VALUE (SPERR_VOICE_PAUSED),
       ADD_VALUE (SPERR_AUDIO_BUFFER_UNDERFLOW),
       ADD_VALUE (SPERR_AUDIO_STOPPED_UNEXPECTEDLY),
       ADD_VALUE (SPERR_NO_WORD_PRONUNCIATION),
       ADD_VALUE (SPERR_ALTERNATES_WOULD_BE_INCONSISTENT),
       ADD_VALUE (SPERR_NOT_SUPPORTED_FOR_SHARED_RECOGNIZER),
       ADD_VALUE (SPERR_TIMEOUT),
       ADD_VALUE (SPERR_REENTER_SYNCHRONIZE),
       ADD_VALUE (SPERR_STATE_WITH_NO_ARCS),
       ADD_VALUE (SPERR_NOT_ACTIVE_SESSION),
       ADD_VALUE (SPERR_ALREADY_DELETED),
       ADD_VALUE (SPERR_RECOXML_GENERATION_FAIL),
       ADD_VALUE (SPERR_SML_GENERATION_FAIL),
       ADD_VALUE (SPERR_NOT_PROMPT_VOICE),
       ADD_VALUE (SPERR_ROOTRULE_ALREADY_DEFINED),
       ADD_VALUE (SPERR_SCRIPT_DISALLOWED),
       ADD_VALUE (SPERR_REMOTE_CALL_TIMED_OUT_START),
       ADD_VALUE (SPERR_REMOTE_CALL_TIMED_OUT_CONNECT),
       ADD_VALUE (SPERR_SECMGR_CHANGE_NOT_ALLOWED),
       ADD_VALUE (SPERR_FAILED_TO_DELETE_FILE),
       ADD_VALUE (SPERR_SHARED_ENGINE_DISABLED),
       ADD_VALUE (SPERR_RECOGNIZER_NOT_FOUND),
       ADD_VALUE (SPERR_AUDIO_NOT_FOUND),
       ADD_VALUE (SPERR_NO_VOWEL),
       ADD_VALUE (SPERR_UNSUPPORTED_PHONEME),
       ADD_VALUE (SPERR_WORD_NEEDS_NORMALIZATION),
       ADD_VALUE (SPERR_CANNOT_NORMALIZE),
       ADD_VALUE (SPERR_TOPIC_NOT_ADAPTABLE),
       ADD_VALUE (SPERR_PHONEME_CONVERSION),
       ADD_VALUE (SPERR_NOT_SUPPORTED_FOR_INPROC_RECOGNIZER),
       ADD_VALUE (SPERR_OVERLOAD),
       ADD_VALUE (SPERR_LEX_INVALID_DATA),
       ADD_VALUE (SPERR_CFG_INVALID_DATA),
       ADD_VALUE (SPERR_LEX_UNEXPECTED_FORMAT),
       ADD_VALUE (SPERR_STRING_TOO_LONG),
       ADD_VALUE (SPERR_STRING_EMPTY),
       ADD_VALUE (SPERR_NON_WORD_TRANSITION),
       ADD_VALUE (SPERR_SISR_ATTRIBUTES_NOT_ALLOWED),
       ADD_VALUE (SPERR_SISR_MIXED_NOT_ALLOWED),
       ADD_VALUE (SPERR_VOICE_NOT_FOUND)
     };

static const char *hr_strerror (HRESULT hr)
{
  static char buf [100];
  const char *name = NULL;

  if (hr == S_OK)
       name = "S_OK";
  else name = search_list_name (hr, hr_errors, DIM(hr_errors));

  if (!name)
     name = "Unknown";
  snprintf (buf, sizeof(buf), "%s/0x%08lX", name, hr);
  return (buf);
}

#else
static const char *hr_strerror (HRESULT hr)
{
  static char buf [100];
  const char *name = (hr == S_OK) ? "S_OK" : NULL;

  if (!name)
     name = "Unknown";
  snprintf (buf, sizeof(buf), "%s/0x%08lX", name, hr);
  return (buf);
}

#endif /* __MINGW64__ || HAVE_SAPI_ERRORS */

static const char *sp_running_state (SPRUNSTATE state)
{
  static const search_list running_states[] = {
                           { 0, "Waiting to speak" },
                           ADD_VALUE (SPRS_DONE),
                           ADD_VALUE (SPRS_IS_SPEAKING)
                         };
  static char buf [100];
  const char *name = search_list_name (state, running_states, DIM(running_states));

  if (!name)
     name = "Unknown";
  snprintf (buf, sizeof(buf), "%s/%d", name, state);
  return (buf);
}

#if defined(TEST)
static void usage (const char *argv0)
{
  printf ("%s [-d] [-vN]  [-VN] <string(s) to speak (with embedded XML-codes)....>\n"
          "  -d:   trace-level; `-dd` more verbose\n"
          "  -v x: use voice x\n"
          "  -V y: use volume y; 0 - 100\n", argv0);
  exit (0);
}

static void halt (int sig)
{
  TRACE (0, "%s()\n", __FUNCTION__);
  g_data.quit = true;
  speak_exit();
  (void) sig;
}

int main (int argc, char **argv)
{
  const char *str;
  int   voice  = 0;
  int   volume = 100;
  int   c, i;

  if (argc == 1)
     usage (argv[0]);

  while ((c = getopt(argc, argv, "dh?v:V:")) != -1)
  {
    switch (c)
    {
      case 'd':
           g_data.trace_level++;
           break;
      case 'v':
           voice = atoi (optarg);
           break;
      case 'V':
           volume = atoi (optarg);
           break;
      case 'h':
      case '?':
      default:
           usage (argv[0]);
           break;
    }
  }

  signal (SIGINT, halt);

  if (!speak_init(voice, volume))
     return (1);

  argv += optind;
  for (i = 0, str = argv[i]; str; str = argv[++i])
  {
    if (i == 0 && !argv[0])
       str = "This is a reasonably long string that should take a while to speak. "
             "This is some more text with <emph>embedded </emph>XML codes.";

    if (!speak_string("%s", str))
    {
      speak_exit();
      return (1);
    }
  }

  while (speak_poll())
     Sleep (500);

  speak_exit();
  return (0);
}

/*
 * To allow 'misc.obj' to link in.
 */
#if defined(__clang__)
  #pragma clang diagnostic ignored "-Wunused-parameter"
#else
  #pragma warning (disable: 4100)
#endif

global_data Modes;

#define DEAD_CODE(ret, rv, func, args)  ret func args { return (rv); }

DEAD_CODE (const char *, "?", mz_version, (void))
DEAD_CODE (const char *, "?", sqlite3_libversion, (void))
DEAD_CODE (const char *, "?", sqlite3_compileoption_get, (int N))
DEAD_CODE (const char *, "?", trace_strerror, (DWORD err))
DEAD_CODE (uint32_t,     0,   rtlsdr_last_error, (void))

#if defined(MG_ENABLE_PACKED_FS) && (MG_ENABLE_PACKED_FS == 1)
  DEAD_CODE (const char *, "?", mg_unpack, (const char *name, size_t *size, time_t *mtime))
  DEAD_CODE (const char *, "?", mg_unlist, (size_t i))
#endif
#endif
