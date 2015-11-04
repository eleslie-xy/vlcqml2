/*****************************************************************************
 * freetype.c : Put text on the video, using freetype2
 *****************************************************************************
 * Copyright (C) 2002 - 2015 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Sigmund Augdal Helberg <dnumgis@videolan.org>
 *          Gildas Bazin <gbazin@videolan.org>
 *          Bernie Purcell <bitmap@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Felix Paul Kühne <fkuehne@videolan.org>
 *          Salah-Eddin Shaban <salshaaban@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_filter.h>

/* Win32 GDI */
#ifdef _WIN32
# include <windows.h>
# include <shlobj.h>
# include <usp10.h>
# include <vlc_charset.h>                                     /* FromT */
# undef HAVE_FONTCONFIG
#endif

#include "../platform_fonts.h"

#if !VLC_WINSTORE_APP
#define FONT_DIR_NT _T("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts")

static inline void AppendFamily( vlc_family_t **pp_list, vlc_family_t *p_family )
{
    while( *pp_list )
        pp_list = &( *pp_list )->p_next;

    *pp_list = p_family;
}


static char *Trim( char *psz_text )
{
    int i_first_char = -1;
    int i_last_char = -1;
    int i_len = strlen( psz_text );

    for( int i = 0; i < i_len; ++i )
    {
        if( psz_text[i] != ' ')
        {
            if( i_first_char == -1 )
                i_first_char = i;
            i_last_char = i;
        }
    }

    psz_text[ i_last_char + 1 ] = 0;
    if( i_first_char != -1 ) psz_text = psz_text + i_first_char;

    return psz_text;
}

static int ConcatenatedIndex( char *psz_haystack, const char *psz_needle )
{
    char *psz_family = psz_haystack;
    char *psz_terminator = psz_haystack + strlen( psz_haystack );
    int i_index = 0;

    while( psz_family < psz_terminator )
    {
        char *psz_amp = strchr( psz_family, '&' );

        if( !psz_amp ) psz_amp = psz_terminator;

        *psz_amp = 0;

        psz_family = Trim( psz_family );
        if( !strcasecmp( psz_family, psz_needle ) )
            return i_index;

        psz_family = psz_amp + 1;
        ++i_index;
    }

    return -1;
}

static int GetFileFontByName( LPCTSTR font_name, char **psz_filename, int *i_index )
{
    HKEY hKey;
    TCHAR vbuffer[MAX_PATH];
    TCHAR dbuffer[256];

    if( RegOpenKeyEx(HKEY_LOCAL_MACHINE, FONT_DIR_NT, 0, KEY_READ, &hKey)
            != ERROR_SUCCESS )
        return 1;

    char *font_name_temp = FromT( font_name );

    for( int index = 0;; index++ )
    {
        DWORD vbuflen = MAX_PATH - 1;
        DWORD dbuflen = 255;

        LONG i_result = RegEnumValue( hKey, index, vbuffer, &vbuflen,
                                      NULL, NULL, (LPBYTE)dbuffer, &dbuflen);
        if( i_result != ERROR_SUCCESS )
        {
            free( font_name_temp );
            RegCloseKey( hKey );
            return i_result;
        }

        char *psz_value = FromT( vbuffer );

        char *s = strchr( psz_value,'(' );
        if( s != NULL && s != psz_value ) s[-1] = '\0';

        int i_concat_idx = 0;
        if( ( i_concat_idx = ConcatenatedIndex( psz_value, font_name_temp ) ) != -1 )
        {
            *i_index = i_concat_idx;
            *psz_filename = FromT( dbuffer );
            free( psz_value );
            break;
        }

        free( psz_value );
    }

    free( font_name_temp );
    RegCloseKey( hKey );
    return 0;
}

static char* GetWindowsFontPath()
{
    wchar_t wdir[MAX_PATH];
    if( S_OK != SHGetFolderPathW( NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, wdir ) )
    {
        GetWindowsDirectoryW( wdir, MAX_PATH );
        wcscat( wdir, L"\\fonts" );
    }
    return FromWide( wdir );
}

static int CALLBACK EnumFontCallback(const ENUMLOGFONTEX *lpelfe, const NEWTEXTMETRICEX *metric,
                                     DWORD type, LPARAM lParam)
{
    VLC_UNUSED( metric );
    if( (type & RASTER_FONTTYPE) ) return 1;

    vlc_family_t *p_family = ( vlc_family_t * ) lParam;

    bool b_bold = ( lpelfe->elfLogFont.lfWeight == FW_BOLD );
    bool b_italic = ( lpelfe->elfLogFont.lfItalic != 0 );

    /*
     * This function will be called by Windows as many times for each font
     * of the family as the number of scripts the font supports.
     * Check to avoid duplicates.
     */
    for( vlc_font_t *p_font = p_family->p_fonts; p_font; p_font = p_font->p_next )
        if( !!p_font->b_bold == !!b_bold && !!p_font->b_italic == !!b_italic )
            return 1;

    char *psz_filename = NULL;
    char *psz_fontfile = NULL;
    int   i_index      = 0;

    if( GetFileFontByName( (LPCTSTR)lpelfe->elfFullName, &psz_filename, &i_index ) )
        return 1;

    if( strchr( psz_filename, DIR_SEP_CHAR ) )
        psz_fontfile = psz_filename;
    else
    {
        /* Get Windows Font folder */
        char *psz_win_fonts_path = GetWindowsFontPath();
        if( asprintf( &psz_fontfile, "%s\\%s", psz_win_fonts_path, psz_filename ) == -1 )
        {
            free( psz_filename );
            free( psz_win_fonts_path );
            return 1;
        }
        free( psz_filename );
        free( psz_win_fonts_path );
    }

    NewFont( psz_fontfile, i_index, b_bold, b_italic, p_family );

    return 1;
}

const vlc_family_t *Win32_GetFamily( filter_t *p_filter, const char *psz_family )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    char *psz_lc = ToLower( psz_family );

    if( unlikely( !psz_lc ) )
        return NULL;

    vlc_family_t *p_family =
        vlc_dictionary_value_for_key( &p_sys->family_map, psz_lc );

    free( psz_lc );

    if( p_family )
        return p_family;

    p_family = NewFamily( p_filter, psz_family, &p_sys->p_families,
                          &p_sys->family_map, psz_family );

    if( unlikely( !p_family ) )
        return NULL;

    LOGFONT lf;
    lf.lfCharSet = DEFAULT_CHARSET;

    LPTSTR psz_fbuffer = ToT( psz_family );
    _tcsncpy( (LPTSTR)&lf.lfFaceName, psz_fbuffer, LF_FACESIZE );
    free( psz_fbuffer );

    /* */
    HDC hDC = GetDC( NULL );
    EnumFontFamiliesEx(hDC, &lf, (FONTENUMPROC)&EnumFontCallback, (LPARAM)p_family, 0);
    ReleaseDC(NULL, hDC);

    return p_family;
}

static int CALLBACK MetaFileEnumProc( HDC hdc, HANDLETABLE* table,
                                      CONST ENHMETARECORD* record,
                                      int table_entries, LPARAM log_font )
{
    VLC_UNUSED( hdc );
    VLC_UNUSED( table );
    VLC_UNUSED( table_entries );

    if( record->iType == EMR_EXTCREATEFONTINDIRECTW )
    {
        const EMREXTCREATEFONTINDIRECTW* create_font_record =
                ( const EMREXTCREATEFONTINDIRECTW * ) record;

        *( ( LOGFONT * ) log_font ) = create_font_record->elfw.elfLogFont;
    }
    return 1;
}

/*
 * This is a hack used by Chrome and WebKit to expose the fallback font used
 * by Uniscribe for some given text for use with custom shapers / font engines.
 */
static char *UniscribeFallback( const char *psz_family, uni_char_t codepoint )
{
    HDC          hdc          = NULL;
    HDC          meta_file_dc = NULL;
    HENHMETAFILE meta_file    = NULL;
    LPTSTR       psz_fbuffer  = NULL;
    char        *psz_result   = NULL;

    hdc = CreateCompatibleDC( NULL );
    if( !hdc )
        return NULL;

    meta_file_dc = CreateEnhMetaFile( hdc, NULL, NULL, NULL );
    if( !meta_file_dc )
        goto error;

    LOGFONT lf;
    memset( &lf, 0, sizeof( lf ) );

    psz_fbuffer = ToT( psz_family );
    if( !psz_fbuffer )
        goto error;
    _tcsncpy( ( LPTSTR ) &lf.lfFaceName, psz_fbuffer, LF_FACESIZE );
    free( psz_fbuffer );

    lf.lfCharSet = DEFAULT_CHARSET;
    HFONT hFont = CreateFontIndirect( &lf );
    if( !hFont )
        goto error;

    HFONT hOriginalFont = SelectObject( meta_file_dc, hFont );

    TCHAR text = codepoint;

    SCRIPT_STRING_ANALYSIS script_analysis;
    HRESULT hresult = ScriptStringAnalyse( meta_file_dc, &text, 1, 0, -1,
                            SSA_METAFILE | SSA_FALLBACK | SSA_GLYPHS | SSA_LINK,
                            0, NULL, NULL, NULL, NULL, NULL, &script_analysis );

    if( SUCCEEDED( hresult ) )
    {
        hresult = ScriptStringOut( script_analysis, 0, 0, 0, NULL, 0, 0, FALSE );
        ScriptStringFree( &script_analysis );
    }

    SelectObject( meta_file_dc, hOriginalFont );
    DeleteObject( hFont );
    meta_file = CloseEnhMetaFile( meta_file_dc );

    if( SUCCEEDED( hresult ) )
    {
        LOGFONT log_font;
        log_font.lfFaceName[ 0 ] = 0;
        EnumEnhMetaFile( 0, meta_file, MetaFileEnumProc, &log_font, NULL );
        if( log_font.lfFaceName[ 0 ] )
            psz_result = FromT( log_font.lfFaceName );
    }

    DeleteEnhMetaFile(meta_file);
    DeleteDC( hdc );
    return psz_result;

error:
    if( meta_file_dc ) DeleteEnhMetaFile( CloseEnhMetaFile( meta_file_dc ) );
    if( hdc ) DeleteDC( hdc );
    return NULL;
}

vlc_family_t *Win32_GetFallbacks( filter_t *p_filter, const char *psz_family,
                                  uni_char_t codepoint )
{
    vlc_family_t  *p_family      = NULL;
    vlc_family_t  *p_fallbacks   = NULL;
    filter_sys_t  *p_sys         = p_filter->p_sys;
    char          *psz_uniscribe = NULL;


    char *psz_lc = ToLower( psz_family );

    if( unlikely( !psz_lc ) )
        return NULL;

    p_fallbacks = vlc_dictionary_value_for_key( &p_sys->fallback_map, psz_lc );

    if( p_fallbacks )
        p_family = SearchFallbacks( p_filter, p_fallbacks, codepoint );

    /*
     * If the fallback list of psz_family has no family which contains the requested
     * codepoint, try UniscribeFallback(). If it returns a valid family which does
     * contain that codepoint, add the new family to the fallback list to speed up
     * later searches.
     */
    if( !p_family )
    {
        psz_uniscribe = UniscribeFallback( psz_lc, codepoint );

        if( !psz_uniscribe )
            goto done;

        const vlc_family_t *p_uniscribe = Win32_GetFamily( p_filter, psz_uniscribe );
        if( !p_uniscribe || !p_uniscribe->p_fonts )
            goto done;

        FT_Face p_face = GetFace( p_filter, p_uniscribe->p_fonts );

        if( !p_face || !FT_Get_Char_Index( p_face, codepoint ) )
            goto done;

        p_family = NewFamily( p_filter, psz_uniscribe, NULL, NULL, NULL );

        if( unlikely( !p_family ) )
            goto done;

        p_family->p_fonts = p_uniscribe->p_fonts;

        if( p_fallbacks )
            AppendFamily( &p_fallbacks, p_family );
        else
            vlc_dictionary_insert( &p_sys->fallback_map,
                                   psz_lc, p_family );
    }

done:
    free( psz_lc );
    free( psz_uniscribe );
    return p_family;
}

char* Dummy_Select( filter_t *p_filter, const char* psz_font,
                    bool b_bold, bool b_italic,
                    int *i_idx, uni_char_t codepoint )
{
    VLC_UNUSED(p_filter);
    VLC_UNUSED(b_bold);
    VLC_UNUSED(b_italic);
    VLC_UNUSED(codepoint);
    VLC_UNUSED(i_idx);

    char *psz_fontname;
    /* Get Windows Font folder */
    char *psz_win_fonts_path = GetWindowsFontPath();
    if( asprintf( &psz_fontname, "%s\\%s", psz_win_fonts_path, psz_font ) == -1 )
    {
        psz_fontname = NULL;
        return NULL;
    }
    free(psz_win_fonts_path);

    return psz_fontname;
}

#endif

