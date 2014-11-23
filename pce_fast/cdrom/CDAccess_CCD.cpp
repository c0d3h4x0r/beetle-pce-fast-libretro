/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "mednafen.h"
#include "general.h"
#include "msvc_compat.h"
#include "CDAccess_CCD.h"

#include <limits>
#include <limits.h>
#include <map>

static void MDFN_strtoupper(std::string &str)
{
   const size_t len = str.length();

   for (size_t x = 0; x < len; x++)
   {
      if (str[x] >= 'a' && str[x] <= 'z')
         str[x] = str[x] - 'a' + 'A';
   }
}

typedef std::map<std::string, std::string> CCD_Section;

template<typename T>
static T CCD_ReadInt(CCD_Section &s, const std::string &propname,
                     const bool have_defval = false, const int defval = 0)
{
   CCD_Section::iterator zit = s.find(propname);

   if (zit == s.end())
   {
      assert(have_defval);
      //      throw MDFN_Error(0, ("Missing property: %s"), propname.c_str());
      return defval;
   }

   const std::string &v = zit->second;
   int scan_base = 10;
   size_t scan_offset = 0;
   long ret = 0;

   if (v.length() >= 3 && v[0] == '0' && v[1] == 'x')
   {
      scan_base = 16;
      scan_offset = 2;
   }

   const char* vp = v.c_str() + scan_offset;
   char* ep = NULL;

   if (std::numeric_limits<T>::is_signed)
      ret = strtol(vp, &ep, scan_base);
   else
      ret = strtoul(vp, &ep, scan_base);

   assert(vp[0] && !ep[0]);
   //      throw MDFN_Error(0, ("Property %s: Malformed integer: %s"), propname.c_str(),
   //                       v.c_str());

   //if(ret < minv || ret > maxv)
   //{
   // throw MDFN_Error(0, ("Property %s: Integer %ld out of range(accepted: %d through %d)."), propname.c_str(), ret, minv, maxv);
   //}

   return ret;
}


CDAccess_CCD::CDAccess_CCD(const char* path) : img_stream(NULL),
   sub_stream(NULL), img_numsectors(0)
{
   try
   {
      Load(path);
   }
   catch (...)
   {
      Cleanup();
      throw;
   }
}

void CDAccess_CCD::Load(const char* path)
{
   FileStream cf(path, FileStream::MODE_READ);
   std::map<std::string, CCD_Section> Sections;
   std::string linebuf;
   std::string cur_section_name;
   std::string dir_path, file_base, file_ext;
   char img_extsd[4] = { 'i', 'm', 'g', 0 };
   char sub_extsd[4] = { 's', 'u', 'b', 0 };

   MDFN_GetFilePathComponents(path, &dir_path, &file_base, &file_ext);

   if (file_ext.length() == 4 && file_ext[0] == '.')
   {
      signed char extupt[3] = { -1, -1, -1 };

      for (int i = 1; i < 4; i++)
      {
         if (file_ext[i] >= 'A' && file_ext[i] <= 'Z')
            extupt[i - 1] = 'A' - 'a';
         else if (file_ext[i] >= 'a' && file_ext[i] <= 'z')
            extupt[i - 1] = 0;
      }

      signed char av = -1;
      for (int i = 0; i < 3; i++)
      {
         if (extupt[i] != -1)
            av = extupt[i];
         else
            extupt[i] = av;
      }

      if (av == -1)
         av = 0;

      for (int i = 0; i < 3; i++)
      {
         if (extupt[i] == -1)
            extupt[i] = av;
      }

      for (int i = 0; i < 3; i++)
      {
         img_extsd[i] += extupt[i];
         sub_extsd[i] += extupt[i];
      }
   }

   //printf("%s %d %d %d\n", file_ext.c_str(), extupt[0], extupt[1], extupt[2]);

   linebuf.reserve(256);

   while (cf.get_line(linebuf) >= 0)
   {
      MDFN_trim(linebuf);

      if (linebuf.length() == 0)  // Skip blank lines.
         continue;

      if (linebuf[0] == '[')
      {
         assert(linebuf.length() > 2 && linebuf[linebuf.length() - 1] == ']');
         //            throw MDFN_Error(0, ("Malformed section specifier: %s"), linebuf.c_str());

         cur_section_name = linebuf.substr(1, linebuf.length() - 2);
         MDFN_strtoupper(cur_section_name);
      }
      else
      {
         const size_t feqpos = linebuf.find('=');
         const size_t leqpos = linebuf.rfind('=');
         std::string k, v;

         assert(feqpos != std::string::npos && feqpos == leqpos);
         //            throw MDFN_Error(0, ("Malformed value pair specifier: %s"), linebuf.c_str());

         k = linebuf.substr(0, feqpos);
         v = linebuf.substr(feqpos + 1);

         MDFN_trim(k);
         MDFN_trim(v);

         MDFN_strtoupper(k);

         Sections[cur_section_name][k] = v;
      }
   }

   {
      CCD_Section &ds = Sections["DISC"];
      unsigned toc_entries = CCD_ReadInt<unsigned>(ds, "TOCENTRIES");
      unsigned num_sessions = CCD_ReadInt<unsigned>(ds, "SESSIONS");
      bool data_tracks_scrambled = CCD_ReadInt<unsigned>(ds, "DATATRACKSSCRAMBLED");

      assert(num_sessions == 1);
      //   throw MDFN_Error(0, ("Unsupported number of sessions: %u"), num_sessions);

      assert(!data_tracks_scrambled);
      //   throw MDFN_Error(0, ("Scrambled CCD data tracks currently not supported."));

      //printf("MOO: %d\n", toc_entries);

      for (unsigned te = 0; te < toc_entries; te++)
      {
         char tmpbuf[64];
         snprintf(tmpbuf, sizeof(tmpbuf), "ENTRY %u", te);
         CCD_Section &ts = Sections[std::string(tmpbuf)];
         unsigned session = CCD_ReadInt<unsigned>(ts, "SESSION");
         uint8 point = CCD_ReadInt<uint8>(ts, "POINT");
         uint8 adr = CCD_ReadInt<uint8>(ts, "ADR");
         uint8 control = CCD_ReadInt<uint8>(ts, "CONTROL");
         uint8 pmin = CCD_ReadInt<uint8>(ts, "PMIN");
         uint8 psec = CCD_ReadInt<uint8>(ts, "PSEC");
         uint8 pframe = CCD_ReadInt<uint8>(ts, "PFRAME");
         signed plba = CCD_ReadInt<signed>(ts, "PLBA");

         assert(session == 1);
         //    throw MDFN_Error(0, "Unsupported TOC entry Session value: %u", session);

         // Reference: ECMA-394, page 5-14
         switch (point)
         {
         default:
            assert(false);
            // throw MDFN_Error(0, "Unsupported TOC entry Point value: %u", point);
            break;

         case 0xA0:
            tocd.first_track = pmin;
            tocd.disc_type = psec;
            break;

         case 0xA1:
            tocd.last_track = pmin;
            break;

         case 0xA2:
            tocd.tracks[100].adr = adr;
            tocd.tracks[100].control = control;
            tocd.tracks[100].lba = plba;
            break;

         case 99:
         case 98:
         case 97:
         case 96:
         case 95:
         case 94:
         case 93:
         case 92:
         case 91:
         case 90:
         case 89:
         case 88:
         case 87:
         case 86:
         case 85:
         case 84:
         case 83:
         case 82:
         case 81:
         case 80:
         case 79:
         case 78:
         case 77:
         case 76:
         case 75:
         case 74:
         case 73:
         case 72:
         case 71:
         case 70:
         case 69:
         case 68:
         case 67:
         case 66:
         case 65:
         case 64:
         case 63:
         case 62:
         case 61:
         case 60:
         case 59:
         case 58:
         case 57:
         case 56:
         case 55:
         case 54:
         case 53:
         case 52:
         case 51:
         case 50:
         case 49:
         case 48:
         case 47:
         case 46:
         case 45:
         case 44:
         case 43:
         case 42:
         case 41:
         case 40:
         case 39:
         case 38:
         case 37:
         case 36:
         case 35:
         case 34:
         case 33:
         case 32:
         case 31:
         case 30:
         case 29:
         case 28:
         case 27:
         case 26:
         case 25:
         case 24:
         case 23:
         case 22:
         case 21:
         case 20:
         case 19:
         case 18:
         case 17:
         case 16:
         case 15:
         case 14:
         case 13:
         case 12:
         case 11:
         case 10:
         case 9:
         case 8:
         case 7:
         case 6:
         case 5:
         case 4:
         case 3:
         case 2:
         case 1:
            tocd.tracks[point].adr = adr;
            tocd.tracks[point].control = control;
            tocd.tracks[point].lba = plba;
            break;
         }
      }
   }

   // Convenience leadout track duplication.
   if (tocd.last_track < 99)
      tocd.tracks[tocd.last_track + 1] = tocd.tracks[100];

   //
   // Open image stream.
   {
      std::string image_path = MDFN_EvalFIP(dir_path,
                                            file_base + std::string(".") + std::string(img_extsd));

      img_stream = new FileStream(image_path.c_str(), FileStream::MODE_READ);

      int64 ss = img_stream->size();

      assert(!(ss % 2352));
      //         throw MDFN_Error(0, ("CCD image size is not evenly divisible by 2352."));

      img_numsectors = ss / 2352;
   }

   //
   // Open subchannel stream
   {
      std::string sub_path = MDFN_EvalFIP(dir_path,
                                          file_base + std::string(".") + std::string(sub_extsd));

      sub_stream = new FileStream(sub_path.c_str(), FileStream::MODE_READ);

      assert(sub_stream->size() == (int64)img_numsectors * 96);
      //         throw MDFN_Error(0, ("CCD SUB file size mismatch."));
   }
}

void CDAccess_CCD::Cleanup(void)
{
   if (img_stream)
   {
      delete img_stream;
      img_stream = NULL;
   }

   if (sub_stream)
   {
      delete sub_stream;
      sub_stream = NULL;
   }
}

CDAccess_CCD::~CDAccess_CCD()
{
   Cleanup();
}

void CDAccess_CCD::Read_Raw_Sector(uint8* buf, int32 lba)
{
   assert(lba >= 0 && (size_t)lba < img_numsectors);
   //      throw(MDFN_Error(0, ("LBA out of range.")));

   uint8 sub_buf[96];

   img_stream->seek(lba * 2352, SEEK_SET);
   img_stream->read(buf, 2352);

   sub_stream->seek(lba * 96, SEEK_SET);
   sub_stream->read(sub_buf, 96);

   subpw_interleave(sub_buf, buf + 2352);
}


void CDAccess_CCD::Read_TOC(CDUtility_TOC* toc)
{
   *toc = tocd;
}

bool CDAccess_CCD::Is_Physical(void) throw()
{
   return false;
}

void CDAccess_CCD::Eject(bool eject_status)
{

}

