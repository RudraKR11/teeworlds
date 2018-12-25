/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <math.h>

#include <base/color.h>
#include <base/system.h>
#include <base/math.h>

#include <engine/graphics.h>
#include <engine/storage.h>
#include <engine/external/json-parser/json.h>
#include <engine/shared/config.h>

#include "skins.h"


const char * const CSkins::ms_apSkinPartNames[NUM_SKINPARTS] = {"body", "marking", "decoration", "hands", "feet", "eyes"}; /* Localize("body","skins");Localize("marking","skins");Localize("decoration","skins");Localize("hands","skins");Localize("feet","skins");Localize("eyes","skins"); */
const char * const CSkins::ms_apColorComponents[NUM_COLOR_COMPONENTS] = {"hue", "sat", "lgt", "alp"};

char *const CSkins::ms_apSkinVariables[NUM_SKINPARTS] = {g_Config.m_PlayerSkinBody, g_Config.m_PlayerSkinMarking, g_Config.m_PlayerSkinDecoration,
													g_Config.m_PlayerSkinHands, g_Config.m_PlayerSkinFeet, g_Config.m_PlayerSkinEyes};
int *const CSkins::ms_apUCCVariables[NUM_SKINPARTS] = {&g_Config.m_PlayerUseCustomColorBody, &g_Config.m_PlayerUseCustomColorMarking, &g_Config.m_PlayerUseCustomColorDecoration,
													&g_Config.m_PlayerUseCustomColorHands, &g_Config.m_PlayerUseCustomColorFeet, &g_Config.m_PlayerUseCustomColorEyes};
int *const CSkins::ms_apColorVariables[NUM_SKINPARTS] = {&g_Config.m_PlayerColorBody, &g_Config.m_PlayerColorMarking, &g_Config.m_PlayerColorDecoration,
													&g_Config.m_PlayerColorHands, &g_Config.m_PlayerColorFeet, &g_Config.m_PlayerColorEyes};


int CSkins::SkinPartScan(const char *pName, int IsDir, int DirType, void *pUser)
{
	CSkins *pSelf = (CSkins *)pUser;
	if(IsDir || !str_endswith(pName, ".png"))
		return 0;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "skins/%s/%s", CSkins::ms_apSkinPartNames[pSelf->m_ScanningPart], pName);
	CImageInfo Info;
	if(!pSelf->Graphics()->LoadPNG(&Info, aBuf, DirType))
	{
		str_format(aBuf, sizeof(aBuf), "failed to load skin part '%s'", pName);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "skins", aBuf);
		return 0;
	}

	CSkinPart Part;
	Part.m_OrgTexture = pSelf->Graphics()->LoadTextureRaw(Info.m_Width, Info.m_Height, Info.m_Format, Info.m_pData, Info.m_Format, 0);
	Part.m_BloodColor = vec3(1.0f, 1.0f, 1.0f);

	unsigned char *d = (unsigned char *)Info.m_pData;
	int Pitch = Info.m_Width*4;

	// dig out blood color
	if(pSelf->m_ScanningPart == SKINPART_BODY)
	{
		int PartX = Info.m_Width/2;
		int PartY = 0;
		int PartWidth = Info.m_Width/2;
		int PartHeight = Info.m_Height/2;

		int aColors[3] = {0};
		for(int y = PartY; y < PartY+PartHeight; y++)
			for(int x = PartX; x < PartX+PartWidth; x++)
			{
				if(d[y*Pitch+x*4+3] > 128)
				{
					aColors[0] += d[y*Pitch+x*4+0];
					aColors[1] += d[y*Pitch+x*4+1];
					aColors[2] += d[y*Pitch+x*4+2];
				}
			}

		Part.m_BloodColor = normalize(vec3(aColors[0], aColors[1], aColors[2]));
	}

	// create colorless version
	int Step = Info.m_Format == CImageInfo::FORMAT_RGBA ? 4 : 3;

	// make the texture gray scale
	for(int i = 0; i < Info.m_Width*Info.m_Height; i++)
	{
		int v = (d[i*Step]+d[i*Step+1]+d[i*Step+2])/3;
		d[i*Step] = v;
		d[i*Step+1] = v;
		d[i*Step+2] = v;
	}

	Part.m_ColorTexture = pSelf->Graphics()->LoadTextureRaw(Info.m_Width, Info.m_Height, Info.m_Format, Info.m_pData, Info.m_Format, 0);
	mem_free(Info.m_pData);

	// set skin part data
	Part.m_Flags = 0;
	if(pName[0] == 'x' && pName[1] == '_')
		Part.m_Flags |= SKINFLAG_SPECIAL;
	if(DirType != IStorage::TYPE_SAVE)
		Part.m_Flags |= SKINFLAG_STANDARD;
	str_truncate(Part.m_aName, sizeof(Part.m_aName), pName, str_length(pName) - 4);
	if(g_Config.m_Debug)
	{
		str_format(aBuf, sizeof(aBuf), "load skin part %s", Part.m_aName);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "skins", aBuf);
	}
	pSelf->m_aaSkinParts[pSelf->m_ScanningPart].add(Part);

	return 0;
}

int CSkins::SkinScan(const char *pName, int IsDir, int DirType, void *pUser)
{
	if(IsDir || !str_endswith(pName, ".json"))
		return 0;

	CSkins *pSelf = (CSkins *)pUser;

	// read file data into buffer
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "skins/%s", pName);
	IOHANDLE File = pSelf->Storage()->OpenFile(aBuf, IOFLAG_READ, IStorage::TYPE_ALL);
	if(!File)
		return 0;
	int FileSize = (int)io_length(File);
	char *pFileData = (char *)mem_alloc(FileSize, 1);
	io_read(File, pFileData, FileSize);
	io_close(File);

	// init
	CSkin Skin = pSelf->m_DummySkin;
	str_truncate(Skin.m_aName, sizeof(Skin.m_aName), pName, str_length(pName) - 5);
	if(pSelf->Find(Skin.m_aName, true) != -1)
		return 0;
	bool SpecialSkin = pName[0] == 'x' && pName[1] == '_';

	// parse json data
	json_settings JsonSettings;
	mem_zero(&JsonSettings, sizeof(JsonSettings));
	char aError[256];
	json_value *pJsonData = json_parse_ex(&JsonSettings, pFileData, FileSize, aError);
	mem_free(pFileData);

	if(pJsonData == 0)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, aBuf, aError);
		return 0;
	}

	// extract data
	const json_value &rStart = (*pJsonData)["skin"];
	if(rStart.type == json_object)
	{
		for(int PartIndex = 0; PartIndex < NUM_SKINPARTS; ++PartIndex)
		{
			const json_value &rPart = rStart[(const char *)ms_apSkinPartNames[PartIndex]];
			if(rPart.type != json_object)
				continue;

			// filename
			const json_value &rFilename = rPart["filename"];
			if(rFilename.type == json_string)
			{
				int SkinPart = pSelf->FindSkinPart(PartIndex, (const char *)rFilename, SpecialSkin);
				if(SkinPart > -1)
					Skin.m_apParts[PartIndex] = pSelf->GetSkinPart(PartIndex, SkinPart);
			}

			// use custom colors
			bool UseCustomColors = false;
			const json_value &rColour = rPart["custom_colors"];
			if(rColour.type == json_string)
			{
				UseCustomColors = str_comp((const char *)rColour, "true") == 0;
			}
			Skin.m_aUseCustomColors[PartIndex] = UseCustomColors;

			// color components
			if(!UseCustomColors)
				continue;

			for(int i = 0; i < NUM_COLOR_COMPONENTS; i++)
			{
				if(PartIndex != SKINPART_MARKING && i == 3)
					continue;

				const json_value &rComponent = rPart[(const char *)ms_apColorComponents[i]];
				if(rComponent.type == json_integer)
				{
					switch(i)
					{
					case 0: Skin.m_aPartColors[PartIndex] = (Skin.m_aPartColors[PartIndex]&0xFF00FFFF) | (rComponent.u.integer << 16); break;
					case 1:	Skin.m_aPartColors[PartIndex] = (Skin.m_aPartColors[PartIndex]&0xFFFF00FF) | (rComponent.u.integer << 8); break;
					case 2: Skin.m_aPartColors[PartIndex] = (Skin.m_aPartColors[PartIndex]&0xFFFFFF00) | rComponent.u.integer; break;
					case 3: Skin.m_aPartColors[PartIndex] = (Skin.m_aPartColors[PartIndex]&0x00FFFFFF) | (rComponent.u.integer << 24); break;
					}
				}
			}
		}
	}

	// clean up
	json_value_free(pJsonData);

	// set skin data
	Skin.m_Flags = SpecialSkin ? SKINFLAG_SPECIAL : 0;
	if(DirType != IStorage::TYPE_SAVE)
		Skin.m_Flags |= SKINFLAG_STANDARD;
	if(g_Config.m_Debug)
	{
		str_format(aBuf, sizeof(aBuf), "load skin %s", Skin.m_aName);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "skins", aBuf);
	}
	pSelf->m_aSkins.add(Skin);

	return 0;
}


void CSkins::OnInit()
{
	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		m_aaSkinParts[p].clear();

		// add none part
		if(p == SKINPART_MARKING || p == SKINPART_DECORATION)
		{
			CSkinPart NoneSkinPart;
			NoneSkinPart.m_Flags = SKINFLAG_STANDARD;
			str_copy(NoneSkinPart.m_aName, "", sizeof(NoneSkinPart.m_aName));
			NoneSkinPart.m_BloodColor = vec3(1.0f, 1.0f, 1.0f);
			m_aaSkinParts[p].add(NoneSkinPart);
		}

		// load skin parts
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "skins/%s", ms_apSkinPartNames[p]);
		m_ScanningPart = p;
		Storage()->ListDirectory(IStorage::TYPE_ALL, aBuf, SkinPartScan, this);

		// add dummy skin part
		if(!m_aaSkinParts[p].size())
		{
			CSkinPart DummySkinPart;
			DummySkinPart.m_Flags = SKINFLAG_STANDARD;
			str_copy(DummySkinPart.m_aName, "dummy", sizeof(DummySkinPart.m_aName));
			DummySkinPart.m_BloodColor = vec3(1.0f, 1.0f, 1.0f);
			m_aaSkinParts[p].add(DummySkinPart);
		}
	}

	// create dummy skin
	m_DummySkin.m_Flags = SKINFLAG_STANDARD;
	str_copy(m_DummySkin.m_aName, "dummy", sizeof(m_DummySkin.m_aName));
	for(int p = 0; p < NUM_SKINPARTS; p++)
	{
		int Default;
		if(p == SKINPART_MARKING || p == SKINPART_DECORATION)
			Default = FindSkinPart(p, "", false);
		else
			Default = FindSkinPart(p, "standard", false);
		if(Default < 0)
			Default = 0;
		m_DummySkin.m_apParts[p] = GetSkinPart(p, Default);
		m_DummySkin.m_aPartColors[p] = p==SKINPART_MARKING ? (255<<24)+65408 : 65408;
		m_DummySkin.m_aUseCustomColors[p] = 0;
	}

	// load skins
	m_aSkins.clear();
	Storage()->ListDirectory(IStorage::TYPE_ALL, "skins", SkinScan, this);

	// add dummy skin
	if(!m_aSkins.size())
		m_aSkins.add(m_DummySkin);

	// add xmas hat
	const char *pFileName = "skins/xmas_hat.png";
	CImageInfo Info;
	if(!Graphics()->LoadPNG(&Info, pFileName, IStorage::TYPE_ALL) || Info.m_Width != 128 || Info.m_Height != 512)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "failed to load xmas hat '%s'", pFileName);
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "game", aBuf);
	}
	else
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "loaded xmas hat '%s'", pFileName);
		Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "game", aBuf);
		m_XmasHatTexture = Graphics()->LoadTextureRaw(Info.m_Width, Info.m_Height, Info.m_Format, Info.m_pData, Info.m_Format, 0);
	}
}

void CSkins::AddSkin(const char *pSkinName)
{
	CSkin Skin = m_DummySkin;
	Skin.m_Flags = 0;
	str_copy(Skin.m_aName, pSkinName, sizeof(Skin.m_aName));
	for(int PartIndex = 0; PartIndex < NUM_SKINPARTS; ++PartIndex)
	{
		int SkinPart = FindSkinPart(PartIndex, ms_apSkinVariables[PartIndex], false);
		if(SkinPart > -1)
			Skin.m_apParts[PartIndex] = GetSkinPart(PartIndex, SkinPart);
		Skin.m_aUseCustomColors[PartIndex] = *ms_apUCCVariables[PartIndex];
		Skin.m_aPartColors[PartIndex] = *ms_apColorVariables[PartIndex];
	}
	int SkinIndex = Find(pSkinName, false);
	if(SkinIndex != -1)
		m_aSkins[SkinIndex] = Skin;
	else
		m_aSkins.add(Skin);
}

void CSkins::RemoveSkin(const CSkin *pSkin)
{
	m_aSkins.remove(*pSkin);
}

int CSkins::Num()
{
	return m_aSkins.size();
}

int CSkins::NumSkinPart(int Part)
{
	return m_aaSkinParts[Part].size();
}

const CSkins::CSkin *CSkins::Get(int Index)
{
	return &m_aSkins[max(0, Index%m_aSkins.size())];
}

int CSkins::Find(const char *pName, bool AllowSpecialSkin)
{
	for(int i = 0; i < m_aSkins.size(); i++)
	{
		if(str_comp(m_aSkins[i].m_aName, pName) == 0 && ((m_aSkins[i].m_Flags&SKINFLAG_SPECIAL) == 0 || AllowSpecialSkin))
			return i;
	}
	return -1;
}

const CSkins::CSkinPart *CSkins::GetSkinPart(int Part, int Index)
{
	int Size = m_aaSkinParts[Part].size();
	return &m_aaSkinParts[Part][max(0, Index%Size)];
}

int CSkins::FindSkinPart(int Part, const char *pName, bool AllowSpecialPart)
{
	for(int i = 0; i < m_aaSkinParts[Part].size(); i++)
	{
		if(str_comp(m_aaSkinParts[Part][i].m_aName, pName) == 0 && ((m_aaSkinParts[Part][i].m_Flags&SKINFLAG_SPECIAL) == 0 || AllowSpecialPart))
			return i;
	}
	return -1;
}

vec3 CSkins::GetColorV3(int v) const
{
	float Dark = DARKEST_COLOR_LGT/255.0f;
	return HslToRgb(vec3(((v>>16)&0xff)/255.0f, ((v>>8)&0xff)/255.0f, Dark+(v&0xff)/255.0f*(1.0f-Dark)));
}

vec4 CSkins::GetColorV4(int v, bool UseAlpha) const
{
	vec3 r = GetColorV3(v);
	float Alpha = UseAlpha ? ((v>>24)&0xff)/255.0f : 1.0f;
	return vec4(r.r, r.g, r.b, Alpha);
}

int CSkins::GetTeamColor(int UseCustomColors, int PartColor, int Team, int Part) const
{
	static const int s_aTeamColors[3] = {0xC4C34E, 0x00FF6B, 0x9BFF6B};

	int TeamHue = (s_aTeamColors[Team+1]>>16)&0xff;
	int TeamSat = (s_aTeamColors[Team+1]>>8)&0xff;
	int TeamLgt = s_aTeamColors[Team+1]&0xff;
	int PartSat = (PartColor>>8)&0xff;
	int PartLgt = PartColor&0xff;

	if(!UseCustomColors)
	{
		PartSat = 255;
		PartLgt = 255;
	}

	int MinSat = 160;
	int MaxSat = 255;

	int h = TeamHue;
	int s = clamp(mix(TeamSat, PartSat, 0.2), MinSat, MaxSat);
	int l = clamp(mix(TeamLgt, PartLgt, 0.2), (int)DARKEST_COLOR_LGT, 200);

	int ColorVal = (h<<16) + (s<<8) + l;
	if(Part == SKINPART_MARKING) // keep alpha
		ColorVal += PartColor&0xff000000;

	return ColorVal;
}
