#pragma once

static bool MaterialTraceFrame = false;
static UInt32 MaterialTraceFrameId = 0;
static UInt32 MaterialTraceDrawIndex = 0;

void (__thiscall* Render)(Main*, BSRenderedTexture*, int, int) = (void (__thiscall*)(Main*, BSRenderedTexture*, int, int))Hooks::Render;
void __fastcall RenderHook(Main* This, UInt32 edx, BSRenderedTexture* RenderedTexture, int Arg2, int Arg3) {
	
	SettingsMainStruct* SettingsMain = &TheSettingManager->SettingsMain;

	TheFrameRateManager->UpdatePerformance();
	TheCameraManager->SetSceneGraph();
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();

	TheShaderManager->UpdateConstants();
	MaterialTraceFrame = TheSettingManager->SettingsMain.Develop.DebugMode &&
		!InterfaceManager->IsActive(Menu::kMenuType_Console) &&
		Global->OnKeyDown(TheSettingManager->SettingsMain.Develop.TraceShaders);
	if (MaterialTraceFrame) {
		MaterialTraceFrameId++;
		MaterialTraceDrawIndex = 0;
		Logger::Log("METALFRAME BEGIN id=%u", MaterialTraceFrameId);
	}
	//if (SettingsMain->Develop.TraceShaders && InterfaceManager->IsActive(Menu::MenuType::kMenuType_None) && Global->OnKeyDown(SettingsMain->Develop.TraceShaders) && DWNode::Get() == NULL) DWNode::Create();
	(*Render)(This, RenderedTexture, Arg2, Arg3);
	if (MaterialTraceFrame) {
		Logger::Log("METALFRAME END id=%u draws=%u", MaterialTraceFrameId, MaterialTraceDrawIndex);
		MaterialTraceFrame = false;
	}

}

namespace {
	struct RuntimeTextureString {
		char* data;
		UInt16 length;
		UInt16 capacity;
	};

	struct RuntimeShaderTextureSet {
		void* vtable;
		UInt32 refCount;
		RuntimeTextureString paths[6];
	};

	static_assert(sizeof(RuntimeTextureString) == 0x08, "Unexpected runtime texture string layout");
	static_assert(sizeof(RuntimeShaderTextureSet) == 0x38, "Unexpected BSShaderTextureSet layout");

	static const UInt32 PPLightingPropertyVtable = 0x010AE0D0;
	static const UInt32 SplitSpecularAlbedoSampler = 8;
	static const UInt32 MetallicSampler = 15;

	// TESR_MaterialMetallic's register, and it must track the declaration in
	// Shaders\Includes\Object.hlsl by hand. This is a RAW per-draw write, not the name-bound
	// RegisterConstant path in ShaderManager, so the constant table cannot redirect it: point it
	// at the wrong slot and it silently overwrites whatever else lives there. c137 upward belongs
	// to SkyAmbient.hlsl (nine float4s for TESR_SkyIrradiance[9] in mode 0).
	static const UInt32 MaterialMetallicRegister = 136;
	static std::map<std::string, IDirect3DBaseTexture9*> MetallicTextureCache;
	static NiGeometry* ActiveMaterialGeometry = nullptr;
	static UInt32 ActiveMaterialPassEnum = 0;
	static bool MaterialDrawHookAttached = false;

	static std::string GetMetallicPath(const char* DiffusePath) {
		if (!DiffusePath || !DiffusePath[0]) return std::string();

		std::string path = DiffusePath;
		std::replace(path.begin(), path.end(), '/', '\\');
		if (_strnicmp(path.c_str(), "Data\\", 5) != 0) {
			if (_strnicmp(path.c_str(), "Textures\\", 9) == 0)
				path.insert(0, "Data\\");
			else
				path.insert(0, "Data\\Textures\\");
		}

		size_t slash = path.find_last_of("\\");
		size_t dot = path.find_last_of('.');
		if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
			path.erase(dot);
		return path + "_metal.dds";
	}

	static IDirect3DBaseTexture9* GetGeometryMetallicTexture(NiGeometry* Geometry) {
		if (!Geometry) return nullptr;
		NiProperty* shade = Geometry->GetProperty(NiProperty::kType_Shade);
		if (!shade || *(UInt32*)shade != PPLightingPropertyVtable) return nullptr;

		BSShaderPPLightingProperty* pp = static_cast<BSShaderPPLightingProperty*>(shade);
		RuntimeShaderTextureSet* set = reinterpret_cast<RuntimeShaderTextureSet*>(pp->spTextureSet);
		if (!set) return nullptr;

		std::string path = GetMetallicPath(set->paths[0].data);
		if (path.empty()) return nullptr;
		std::string key = path;
		std::transform(key.begin(), key.end(), key.begin(), ::tolower);
		auto found = MetallicTextureCache.find(key);
		if (found != MetallicTextureCache.end()) return found->second;

		IDirect3DBaseTexture9* texture = nullptr;
		if (std::filesystem::exists(path))
			texture = TheTextureManager->GetFileTexture(path, TextureRecord::PlanarBuffer);
		MetallicTextureCache[key] = texture;
		return texture;
	}

	static IDirect3DBaseTexture9* GetGeometryDiffuseTexture(NiGeometry* Geometry) {
		if (!Geometry) return nullptr;
		NiProperty* shade = Geometry->GetProperty(NiProperty::kType_Shade);
		if (!shade || *(UInt32*)shade != PPLightingPropertyVtable) return nullptr;

		BSShaderPPLightingProperty* pp = static_cast<BSShaderPPLightingProperty*>(shade);
		if (!pp->ppTextures[0] || !*pp->ppTextures[0]) return nullptr;

		NiSourceTexture* diffuse = *pp->ppTextures[0];
		return diffuse->rendererData ? diffuse->rendererData->dTexture : nullptr;
	}

	static bool IsSplitSpecularPass(UInt32 PassEnum) {
		// BSSM_2x_SPECULARDIR through BSSM_2x_SPECULARPT3_SbShp. These are the
		// ONLY_SPECULAR permutations selected when the engine decomposes a material.
		return PassEnum >= 346 && PassEnum <= 375;
	}

}

typedef HRESULT (STDMETHODCALLTYPE* DrawIndexedPrimitive_t)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
	INT, UINT, UINT, UINT, UINT);
static DrawIndexedPrimitive_t DrawIndexedPrimitiveOriginal = nullptr;

static HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveHook(IDirect3DDevice9* Device,
	D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
	UINT StartIndex, UINT PrimitiveCount) {
	NiGeometry* geometry = ActiveMaterialGeometry;
	NiProperty* shade = geometry ? geometry->GetProperty(NiProperty::kType_Shade) : nullptr;
	if (!shade || *(UInt32*)shade != PPLightingPropertyVtable)
		return DrawIndexedPrimitiveOriginal(Device, PrimitiveType, BaseVertexIndex, MinVertexIndex,
			NumVertices, StartIndex, PrimitiveCount);

	IDirect3DBaseTexture9* previousTexture = nullptr;
	IDirect3DBaseTexture9* previousSplitAlbedo = nullptr;
	float previousConstant[4];
	DWORD previousSampler[6];
	DWORD previousSplitAlbedoSampler[6];
	const D3DSAMPLERSTATETYPE samplerTypes[6] = {
		D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER,
		D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE
	};
	Device->GetTexture(MetallicSampler, &previousTexture);
	Device->GetPixelShaderConstantF(MaterialMetallicRegister, previousConstant, 1);
	for (int i = 0; i < 6; i++)
		Device->GetSamplerState(MetallicSampler, samplerTypes[i], &previousSampler[i]);

	IDirect3DBaseTexture9* metallic = GetGeometryMetallicTexture(geometry);

	// Every split-specular draw needs the albedo, whether or not the geometry carries a metallic
	// map. The engine's ONLY_SPECULAR permutations bind NormalMap to s0 and no diffuse at all,
	// because vanilla specular is gloss * pow(NdotH, p) -- a colourless highlight that never
	// looks at albedo. PBR reflectance is lerp(0.04, albedo, metallicness), so at high
	// metallicness the albedo IS the specular colour, and a pass that cannot see it renders
	// white. The engine re-decomposes lighting per draw as the light set changes, so the same
	// surface alternates between albedo-tinted and white specular from one frame to the next.
	const bool splitSpecular = IsSplitSpecularPass(ActiveMaterialPassEnum);
	IDirect3DBaseTexture9* splitAlbedo = splitSpecular ? GetGeometryDiffuseTexture(geometry) : nullptr;
	if (splitSpecular) {
		Device->GetTexture(SplitSpecularAlbedoSampler, &previousSplitAlbedo);
		for (int i = 0; i < 6; i++)
			Device->GetSamplerState(SplitSpecularAlbedoSampler, samplerTypes[i], &previousSplitAlbedoSampler[i]);
	}

	// Diagnose the saloon test material's complete draw composition. Log only distinct state
	// combinations so a reproduced camera sweep remains readable.
	RuntimeShaderTextureSet* textureSet = reinterpret_cast<RuntimeShaderTextureSet*>(
		static_cast<BSShaderPPLightingProperty*>(shade)->spTextureSet);
	std::string metallicPath = textureSet ? GetMetallicPath(textureSet->paths[0].data) : std::string();
	if (metallicPath.find("ProspectorSaloon_metal.dds") != std::string::npos ||
		metallicPath.find("prospectorsaloon_metal.dds") != std::string::npos) {
		DWORD alphaBlend, srcBlend, dstBlend, blendOp, separateAlpha;
		DWORD srcBlendAlpha, dstBlendAlpha, blendOpAlpha, colorWrite, zWrite;
		Device->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
		Device->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
		Device->GetRenderState(D3DRS_DESTBLEND, &dstBlend);
		Device->GetRenderState(D3DRS_BLENDOP, &blendOp);
		Device->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &separateAlpha);
		Device->GetRenderState(D3DRS_SRCBLENDALPHA, &srcBlendAlpha);
		Device->GetRenderState(D3DRS_DESTBLENDALPHA, &dstBlendAlpha);
		Device->GetRenderState(D3DRS_BLENDOPALPHA, &blendOpAlpha);
		Device->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
		Device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);

		NiD3DPass* activePass = *(NiD3DPass**)0x0126F74C;
		const char* pixelName = activePass && activePass->PixelShader && activePass->PixelShader->Name
			? activePass->PixelShader->Name : "(unknown)";
		char stateKey[256];
		sprintf_s(stateKey, "%u|%s|%u|%u|%u|%u|%u|%u|%u|%u|%u|%u",
			ActiveMaterialPassEnum, pixelName, alphaBlend, srcBlend, dstBlend, blendOp,
			separateAlpha, srcBlendAlpha, dstBlendAlpha, blendOpAlpha, colorWrite, zWrite);
		static std::map<std::string, bool> seenStates;
		if (!seenStates[stateKey]) {
			seenStates[stateKey] = true;
			Logger::Log("METALPASS enum=%u desc=%s ps=%s blend=%u src=%u dst=%u op=%u sepA=%u srcA=%u dstA=%u opA=%u write=%08X zwrite=%u",
				ActiveMaterialPassEnum,
				Pointers::Functions::GetPassDescription(ActiveMaterialPassEnum), pixelName,
				alphaBlend, srcBlend, dstBlend, blendOp, separateAlpha,
				srcBlendAlpha, dstBlendAlpha, blendOpAlpha, colorWrite, zWrite);
		}

		if (MaterialTraceFrame) {
			MaterialTraceDrawIndex++;
			const char* geometryName = geometry->m_pcName ? geometry->m_pcName : "(unnamed)";
			Logger::Log("METALDRAW frame=%u order=%u geo=%08X name=%s enum=%u desc=%s ps=%s blend=%u src=%u dst=%u op=%u sepA=%u srcA=%u dstA=%u opA=%u write=%08X zwrite=%u",
				MaterialTraceFrameId, MaterialTraceDrawIndex, (UInt32)geometry, geometryName,
				ActiveMaterialPassEnum,
				Pointers::Functions::GetPassDescription(ActiveMaterialPassEnum), pixelName,
				alphaBlend, srcBlend, dstBlend, blendOp, separateAlpha,
				srcBlendAlpha, dstBlendAlpha, blendOpAlpha, colorWrite, zWrite);
		}
	}

	// .x metallic map bound to s15. .y enables the raw-mask diagnostic for mapped materials,
	// which deliberately bypasses lighting in the replacement shader so angular changes can only
	// come from binding, UV/sampling, or pass selection -- not the BRDF or shadows. .z albedo
	// bound to s8, which answers a different question than .x: a split-specular draw needs the
	// albedo regardless of whether the material has a metallic map, and GetGeometryDiffuseTexture
	// can still come back empty for geometry whose diffuse has not been realised.
	float metallicPresent[4] = {
		metallic ? 1.0f : 0.0f,
		0.0f,
		splitAlbedo ? 1.0f : 0.0f,
		0.0f
	};
	Device->SetTexture(MetallicSampler, metallic);
	Device->SetPixelShaderConstantF(MaterialMetallicRegister, metallicPresent, 1);
	if (splitSpecular) {
		// Mirror the engine's own sampler rather than restating it. The alias has to sample the
		// way the engine samples this same texture where it binds it normally, because at
		// metallicness 1 reflectance IS the albedo: any disagreement between the split and
		// combined decompositions is the entire surface, not a correction to it. Hardcoding got
		// D3DSAMP_SRGBTEXTURE wrong -- the engine samples raw and the alias linearised, handing
		// the split pass albedo^2.2 and making metal jump in brightness whenever the engine
		// re-decomposed the lighting. Copying leaves nothing to drift out of step.
		for (int i = 0; i < 6; i++) {
			DWORD engineState = 0;
			Device->GetSamplerState(0, samplerTypes[i], &engineState);
			Device->SetSamplerState(SplitSpecularAlbedoSampler, samplerTypes[i], engineState);
		}
		Device->SetTexture(SplitSpecularAlbedoSampler, splitAlbedo);
	}
	if (metallic) {
		Device->SetSamplerState(MetallicSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		Device->SetSamplerState(MetallicSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		Device->SetSamplerState(MetallicSampler, D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
		Device->SetSamplerState(MetallicSampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
		Device->SetSamplerState(MetallicSampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
		Device->SetSamplerState(MetallicSampler, D3DSAMP_SRGBTEXTURE, 0);
	}

	HRESULT result = DrawIndexedPrimitiveOriginal(Device, PrimitiveType, BaseVertexIndex,
		MinVertexIndex, NumVertices, StartIndex, PrimitiveCount);

	Device->SetTexture(MetallicSampler, previousTexture);
	Device->SetPixelShaderConstantF(MaterialMetallicRegister, previousConstant, 1);
	for (int i = 0; i < 6; i++)
		Device->SetSamplerState(MetallicSampler, samplerTypes[i], previousSampler[i]);
	if (splitSpecular) {
		Device->SetTexture(SplitSpecularAlbedoSampler, previousSplitAlbedo);
		for (int i = 0; i < 6; i++)
			Device->SetSamplerState(SplitSpecularAlbedoSampler, samplerTypes[i], previousSplitAlbedoSampler[i]);
	}
	if (previousTexture) previousTexture->Release();
	if (previousSplitAlbedo) previousSplitAlbedo->Release();
	return result;
}

void AttachMaterialDrawHook() {
	if (MaterialDrawHookAttached || !TheRenderManager || !TheRenderManager->device) return;
	void** vtable = *(void***)TheRenderManager->device;
	DrawIndexedPrimitiveOriginal = (DrawIndexedPrimitive_t)vtable[82];
	if (!DrawIndexedPrimitiveOriginal) return;

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)DrawIndexedPrimitiveOriginal, DrawIndexedPrimitiveHook);
	if (DetourTransactionCommit() == NO_ERROR)
		MaterialDrawHookAttached = true;
}

void (__thiscall* SetShaders)(BSShader*, UInt32) = (void (__thiscall*)(BSShader*, UInt32))Hooks::SetShaders;
void __fastcall SetShadersHook(BSShader* This, UInt32 edx, UInt32 PassIndex) {
	
	NiGeometry* Geometry = *(NiGeometry**)(*(void**)0x011F91E0);
	NiD3DPass* Pass = *(NiD3DPass**)0x0126F74C;
	NiD3DVertexShaderEx* VertexShader = (NiD3DVertexShaderEx*)Pass->VertexShader;
	NiD3DPixelShaderEx* PixelShader = (NiD3DPixelShaderEx*)Pass->PixelShader;
	IDirect3DVertexShader9* VertexShader2 = TheRenderManager->renderState->GetVertexShader();
	IDirect3DPixelShader9* PixelShader2 = TheRenderManager->renderState->GetPixelShader();

	if (VertexShader) {
		VertexShader->SetupShader(VertexShader2);
	}
	else {
		Logger::Log("Error getting vertex shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	}
	if (PixelShader) {
		PixelShader->SetupShader(PixelShader2);
	}
	else {
		Logger::Log("Error getting pixel shader for pass %s", Pointers::Functions::GetPassDescription(PassIndex));
	}

	// trace pipeline active shaders
	if (TheSettingManager->SettingsMain.Develop.DebugMode && !InterfaceManager->IsActive(Menu::MenuType::kMenuType_Console) && Global->OnKeyDown(TheSettingManager->SettingsMain.Develop.TraceShaders)) {
		char Name[256];
		sprintf(Name, "Pass %i %s, %s (%s %s)", PassIndex, Pointers::Functions::GetPassDescription(PassIndex), Geometry->m_pcName, VertexShader->Name, PixelShader->Name);
		if (VertexShader->ShaderHandle == VertexShader->ShaderHandleBackup) strcat(Name, " - Vertex: vanilla");
		if (PixelShader->ShaderHandle == PixelShader->ShaderHandleBackup) strcat(Name, " - Pixel: vanilla");
		Logger::Log("%s", Name);
		InterfaceManager->ShowMessage("Shaders Traced");
		//DWNode::AddNode(Name, Geometry->m_parent, Geometry);
	}
	(*SetShaders)(This, PassIndex);

}

void (__cdecl* RenderGeometryPass)(void*, UInt32, int, int, int) =
	(void (__cdecl*)(void*, UInt32, int, int, int))Hooks::RenderGeometryPass;
void __cdecl RenderGeometryPassHook(void* Pass, UInt32 PassEnum, int Arg3, int Arg4, int Arg5) {
	NiGeometry* previousGeometry = ActiveMaterialGeometry;
	UInt32 previousPassEnum = ActiveMaterialPassEnum;
	ActiveMaterialGeometry = Pass ? *(NiGeometry**)Pass : NULL;
	ActiveMaterialPassEnum = PassEnum;
	(*RenderGeometryPass)(Pass, PassEnum, Arg3, Arg4, Arg5);
	ActiveMaterialGeometry = previousGeometry;
	ActiveMaterialPassEnum = previousPassEnum;
}

HRESULT (__thiscall* SetSamplerState)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8) = (HRESULT (__thiscall*)(NiDX9RenderState*, UInt32, D3DSAMPLERSTATETYPE, UInt32, UInt8))Hooks::SetSamplerState;
HRESULT __fastcall SetSamplerStateHook(NiDX9RenderState* This, UInt32 edx, UInt32 Sampler, D3DSAMPLERSTATETYPE Type, UInt32 Value, UInt8 Save) {

	UInt16* TypeMap = (UInt16*)0x126F92C;
	HRESULT r = D3D_OK;

	if (TypeMap[Type] < 5)
		r = (*SetSamplerState)(This, Sampler, Type, Value, Save);
	else
		r = TheRenderManager->device->SetSamplerState(Sampler, Type, Value);
	return r;

}

void (__thiscall* RenderWorldSceneGraph)(Main*, Sun*, UInt8, UInt8, UInt8) = (void (__thiscall*)(Main*, Sun*, UInt8, UInt8, UInt8))Hooks::RenderWorldSceneGraph;
void __fastcall RenderWorldSceneGraphHook(Main* This, UInt32 edx, Sun* SkySun, UInt8 IsFirstPerson, UInt8 WireFrame, UInt8 Arg4) {
	(*RenderWorldSceneGraph)(This, SkySun, IsFirstPerson, WireFrame, Arg4);

	const bool bPipBoyOpen = InterfaceManager->IsPipBoyOpen();
	const bool bPipBoyLive = (TheGameMenuManager->IsLiveMenu && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE);

	if (!bPipBoyOpen || bPipBoyLive)
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTexture); // disable updating the world buffer when pipboy is out

	if (!IsFirstPerson) {
		// clear the viewmodel depth buffer
		TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
		TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
	}
}

void (__thiscall* RenderFirstPerson)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*) = (void (__thiscall*)(Main*, NiDX9Renderer*, NiGeometry*, Sun*, BSRenderedTexture*))Hooks::RenderFirstPerson;
void __fastcall RenderFirstPersonHook(Main* This, UInt32 edx, NiDX9Renderer* Renderer, NiGeometry* Geo, Sun* SkySun, BSRenderedTexture* RenderedTexture) {
	// Clear the depth buffer before rendering first person model to prevent clipping with world objects & other artefacts
	TheRenderManager->Clear(NULL, NiRenderer::kClear_ZBUFFER);
	//ThisCall(0x00874C10, Global);
	(*RenderFirstPerson)(This, Renderer, Geo, SkySun, RenderedTexture);
	TheRenderManager->ResolveDepthBuffer(TheTextureManager->DepthTextureViewModel);
}

void (__thiscall* RenderReflections)(WaterManager*, NiCamera*, ShadowSceneNode*) = (void (__thiscall*)(WaterManager*, NiCamera*, ShadowSceneNode*))Hooks::RenderReflections;
void __fastcall RenderReflectionsHook(WaterManager* This, UInt32 edx, NiCamera* Camera, ShadowSceneNode* SceneNode) {
	
	D3DXVECTOR4* ShadowData = &TheShaderManager->Effects.ShadowsExteriors->Constants.Data;
	float ShadowDataBackup = ShadowData->x;

	D3DXVECTOR4* TerrainParallaxData = &TheShaderManager->Shaders.Terrain->ParallaxConstants.Data;
	float TerrainParallaxBackup = TerrainParallaxData->x;

	if (DWNode::Get()) DWNode::AddNode("BEGIN REFLECTIONS RENDERING", NULL, NULL);
	ShadowData->x = -1.0f; // Disables the shadows rendering for water reflections (the geo is rendered with the same shaders used in the normal scene!)
	TerrainParallaxData->x = 0;
	(*RenderReflections)(This, Camera, SceneNode);
	ShadowData->x = ShadowDataBackup;
	TerrainParallaxData->x = TerrainParallaxBackup;
	if (DWNode::Get()) DWNode::AddNode("END REFLECTIONS RENDERING", NULL, NULL);
}

void (__thiscall* RenderPipboy)(Main*, NiGeometry*, NiDX9Renderer*) = (void (__thiscall*)(Main*, NiGeometry*, NiDX9Renderer*))Hooks::RenderPipboy;
void __fastcall RenderPipboyHook(Main* This, UInt32 edx, NiGeometry* Geo, NiDX9Renderer* Renderer) {
	WorldSceneGraph->UpdateParticleShaderFoV(Player->firstPersonFoV);
//	Player->SetFoV(Player->firstPersonFoV);
	(*RenderPipboy)(This, Geo, Renderer);
}

float (__thiscall* GetWaterHeightLOD)(TESWorldSpace*) = (float (__thiscall*)(TESWorldSpace*))Hooks::GetWaterHeightLOD;
float __fastcall GetWaterHeightLODHook(TESWorldSpace* This, UInt32 edx) {
	
	float r = This->waterHeight;
	if (*(void**)This == (void*)0x0103195C) r = TheShaderManager->Shaders.Water->Constants.Default.waterSettings.x;
	return r;

}

bool bSkippedRender_RenderedMenu = false;
bool bDoneRender_LockPickMenu = false;

void(__cdecl* ProcessImageSpaceShaders)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*) = (void(__cdecl*)(NiDX9Renderer*, BSRenderedTexture*, BSRenderedTexture*))Hooks::ProcessImageSpaceShaders;
void __cdecl ProcessImageSpaceShadersHook(NiDX9Renderer* Renderer, BSRenderedTexture* SourceTarget, BSRenderedTexture* DestinationTarget) {
	bool bLiveRenderedMenu = false; // FORenderedMenu, FOPipBoyManager
	bool bLive3DMenu = false; // Normal menus, but 3D, lockpick etc
	if (TESMain::IsMenuBackgroundReady() && TheGameMenuManager->IsLiveMenu && InterfaceManager->currentMode != 1) {
		const bool bLockPickMenu = LockPickMenu::GetSingleton() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_LockPick, false, false) == GameMenuManager::MENU_LIVE;
		const bool bPipBoyLive = InterfaceManager->IsPipBoyOpen() && TheGameMenuManager->IsLiveMenu(Menu::kMenuType_BigFour, false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		const bool bRenderedMenuLive = InterfaceManager->pRenderedMenu && TheGameMenuManager->IsLiveMenu(InterfaceManager->menuStack[0], false, false) == GameMenuManager::MenuPauseState::MENU_LIVE;
		bLiveRenderedMenu = bPipBoyLive || bRenderedMenuLive;
		bLive3DMenu = bLockPickMenu;
	}

	if (bLive3DMenu) {
		if (bDoneRender_LockPickMenu) {
			bDoneRender_LockPickMenu = false;
			ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
			return;
		}
		else {
			bDoneRender_LockPickMenu = true;
		}
	}
	else {
		bDoneRender_LockPickMenu = false;
	}
	
	if (bLiveRenderedMenu) {
		if (!bSkippedRender_RenderedMenu) {
			bSkippedRender_RenderedMenu = true;
			ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);
			return;
		}
		else {
			bSkippedRender_RenderedMenu = false;
		}
	}
	else {
		bSkippedRender_RenderedMenu = false;
	}

	IDirect3DDevice9* Device = TheRenderManager->device;
	NiDX9RenderState* RenderState = TheRenderManager->renderState;
	IDirect3DSurface9* GameSurface = NULL;
	IDirect3DSurface9* OutputSurface = NULL;
	
	TheRenderManager->UpdateSceneCameraData();
	TheRenderManager->SetupSceneCamera();
	TheShaderManager->UpdateConstants();

	if (SourceTarget && TheSettingManager->SettingsMain.Main.RenderPreTonemapping) {
		SourceTarget->GetD3DTexture(0)->GetSurfaceLevel(0, &GameSurface); // get the surface from the game render target

		// Disable render state settings that create artefacts
		RenderState->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ZWRITEENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILMASK, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILWRITEMASK, 255, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILREF, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_COLORWRITEENABLE, 15, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHATESTENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHABLENDENABLE, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_ALPHAREF, 0, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_NORMALIZENORMALS, D3DZB_FALSE, RenderStateArgs);
		RenderState->SetRenderState(D3DRS_POINTSIZE, 810365505, RenderStateArgs); // fix flickering linked to alpha somehow

		TheShaderManager->RenderEffectsPreTonemapping(GameSurface);
	
	}

	ProcessImageSpaceShaders(Renderer, SourceTarget, DestinationTarget);

	if (!DestinationTarget && TheRenderManager->currentRTGroup) {
		OutputSurface = TheRenderManager->currentRTGroup->RenderTargets[0]->data->Surface;
		if (!TheSettingManager->SettingsMain.Main.RenderPreTonemapping) TheShaderManager->RenderEffectsPreTonemapping(OutputSurface);
		TheShaderManager->RenderEffects(OutputSurface);
		TheRenderManager->CheckAndTakeScreenShot(OutputSurface, TheSettingManager->SettingsMain.Main.HDRScreenshot);
	}

	if (GameSurface) GameSurface->Release();
}

static void RenderMainMenuMovie() {

	if (TheSettingManager->SettingsMain.Main.ReplaceIntro && InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main))
		TheBinkManager->Render(MainMenuMovie);
	else
		TheBinkManager->Close();

}

CallDetour kRenderInterfaceDetour;
void __fastcall RenderInterfaceHook(void* apThis, void*, void* apCuller, bool abPipboyVisible) {
	RenderMainMenuMovie();
	ImGuiManager::NewFrame();
	ThisCall(kRenderInterfaceDetour.GetOverwrittenAddr(), apThis, apCuller, abPipboyVisible);
	ImGuiManager::Render();
}

static void SetTileShaderConstants() {
	
	float ViewProj[16];
	NiVector4 TintColor = { 1.0f, 1.0f, 1.0f, 0.0f };

	if (InterfaceManager->IsActive(Menu::MenuType::kMenuType_Main)) {
		TheRenderManager->device->GetVertexShaderConstantF(0, ViewProj, 4);
		if ((int)ViewProj[3] == -1 && (int)ViewProj[7] == 1 && (int)ViewProj[15] == 1) TheRenderManager->device->SetPixelShaderConstantF(0, (const float*)&TintColor, 1);
	}

}

__declspec(naked) void SetTileShaderConstantsHook() {

	__asm {
		pushad
		call	SetTileShaderConstants
		popad
		cmp		byte ptr [esi + 0xAC], 0
		jmp		Jumpers::SetTileShaderConstants::Return
	}

}

void* (__thiscall* ShowDetectorWindow)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int) = (void* (__thiscall*)(DetectorWindow*, HWND, HINSTANCE, NiNode*, char*, int, int, int, int))::Hooks::ShowDetectorWindow;
void* __fastcall ShowDetectorWindowHook(DetectorWindow* This, UInt32 edx, HWND Handle, HINSTANCE Instance, NiNode* RootNode, char* FormCaption, int X, int Y, int Width, int Height) {
	
	NiAVObject* Object = NULL;
	void* r = NULL;

	r = (ShowDetectorWindow)(This, Handle, Instance, RootNode, (char*)"Pipeline detector by Alenet", X, Y, 1280, 1024);
	for (int i = 0; i < RootNode->m_children.end; i++) {
		NiNode* Node = (NiNode*)RootNode->m_children.data[i];
		Node->m_children.data[0] = NULL;
		Node->m_children.data[1] = NULL;
		Node->m_children.end = 0;
		Node->m_children.numObjs = 0;
	}
	return r;

}

void DetectorWindowSetNodeName(char* Buffer, int Size, char* Format, char* ClassName, char* Name, float LPosX, float LPosY, float LPosZ) {

	sprintf(Buffer, "%s", Name);

}

static void DetectorWindowCreateTreeView(HWND TreeView) {

	HFONT Font = CreateFontA(14, 0, 0, 0, FW_DONTCARE, NULL, NULL, NULL, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
	SendMessageA(TreeView, WM_SETFONT, (WPARAM)Font, TRUE);
	SendMessageA(TreeView, TVM_SETBKCOLOR, NULL, 0x001E1E1E);
	SendMessageA(TreeView, TVM_SETTEXTCOLOR, NULL, 0x00DCDCDC);

}

__declspec(naked) void DetectorWindowCreateTreeViewHook() {

	__asm {
		pushad
		push	eax
		call	DetectorWindowCreateTreeView
		pop		eax
		popad
		mov     ecx, [ebp - 0x48]
		mov		[ecx + 0x0C], eax
		mov     esp, ebp
		pop     ebp
		jmp		Jumpers::DetectorWindow::CreateTreeViewReturn
	}

}

void DetectorWindowDumpAttributes(HWND TreeView, UInt32 Msg, WPARAM wParam, LPTVINSERTSTRUCTA lParam) {

	TVITEMEXA Item = { NULL };
	char T[260] = { '\0' };

	Item.pszText = T;
	Item.mask = TVIF_TEXT;
	Item.hItem = (HTREEITEM)SendMessageA(TreeView, TVM_GETNEXTITEM, TVGN_PARENT, (LPARAM)lParam->hParent);
	Item.cchTextMax = 260;
	SendMessageA(TreeView, TVM_GETITEMA, 0, (LPARAM)&Item);
	if (!memcmp(Item.pszText, "Pass", 4))
		SendMessageA(TreeView, TVM_DELETEITEM, 0, (LPARAM)lParam->hParent);
	else
		if (strlen(Item.pszText)) SendMessageA(TreeView, Msg, wParam, (LPARAM)lParam);

}

__declspec(naked) void DetectorWindowDumpAttributesHook() {

	__asm {
		call	DetectorWindowDumpAttributes
		add		esp, 16
		jmp		Jumpers::DetectorWindow::DumpAttributesReturn
	}

}

__declspec(naked) void DetectorWindowConsoleCommandHook() {

	__asm {
		call	DWNode::Create
		jmp		Jumpers::DetectorWindow::ConsoleCommandReturn
	}

}

// Enables culling of muzzle flashes so they don't stay after firing
void __fastcall MuzzleLightCullingFix(MuzzleFlash* This) {
	if (This->light) {
		if (!This->bEnabled) {
			This->light->m_flags |= 1;
		}
		else {
			This->light->m_flags &= ~1;
		}
	}
	ThisCall(0x9BB8A0, This);
}

BSFogProperty* __cdecl ShadowSceneNode__GetFogPropertyEx(UInt32 aeType) {
	if (TheShaderManager->Effects.VolumetricFog->Enabled && !TheShaderManager->GameState.isUnderwater) {
		aeType = 1;  // Use UI scene node and thus render no fog.
	}

	return BSShaderManager::GetShadowSceneNode(static_cast<BSShaderManager::SceneGraphType>(aeType))->fogProperty;
}

NiPoint2* __fastcall WaterFogRemover(NiPoint2* point, void*, float x, float y)
{
	point->x = 0.f;
	point->y = 0.f;
	return point;
}

// Compatibility patch for DXVK 16bits buffer upgrade.
typedef bool(__cdecl* DisableFormatUpgradeFunc)();
typedef bool(__cdecl* EnableFormatUpgradeFunc)();

BSRenderedTexture* (__cdecl* CreateBSRenderedTexture)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32) = (BSRenderedTexture * (__cdecl*)(BSString*, const UInt32, const UInt32, NiTexture::FormatPrefs*, UInt32, bool, NiDepthStencilBuffer*, UInt32, UInt32))Hooks::CreateRenderedTexture;
BSRenderedTexture* __cdecl CreateSaveTextureHook(BSString* apName, const UInt32 uiWidth, const UInt32 uiHeight, NiTexture::FormatPrefs* kPrefs, 
	UInt32 eMSAAPref, bool bUseDepthStencil, NiDepthStencilBuffer* pkDSBuffer, UInt32 a7, UInt32 uiBackgroundColor) {
	HMODULE hDLL = GetModuleHandle(L"d3d9.dll");

	// If the loaded library is DXVK-HDR (https://github.com/EndlesslyFlowering/dxvk), these will pass
	DisableFormatUpgradeFunc disable = (DisableFormatUpgradeFunc)GetProcAddress(hDLL, "DXVK_D3D9_HDR_DisableRenderTargetUpgrade");
	EnableFormatUpgradeFunc enable = (DisableFormatUpgradeFunc)GetProcAddress(hDLL, "DXVK_D3D9_HDR_EnableRenderTargetUpgrade");

	if (disable)
		disable(); // Temporarily disable the format upgrade for the texture
	BSRenderedTexture* pTexture = CreateBSRenderedTexture(apName, uiWidth, uiHeight, kPrefs, eMSAAPref, bUseDepthStencil, pkDSBuffer, a7, uiBackgroundColor);
	if (enable)
		enable(); // Restore the format upgrade functionality 

	return pTexture;
}

// 0xE69660
bool __fastcall NiDX9Renderer__Do_EndFrame(NiDX9Renderer* apThis, void*) {
	bool bResult = ThisStdCall<bool>(0xE69660, apThis);

	// Reload effects if queued.
	if (TheShaderManager && TheShaderManager->EffectReloadQueued) {
		TheShaderManager->ReloadEffects();
		TheShaderManager->EffectReloadQueued = false;
	}

	return bResult;
}


// Code to increase all lights strength
//__forceinline NiVector4* GetConstant(int index) {
//	return &((NiVector4*)0x11FA0C0)[index];
//}
//
//__forceinline NiColorAlpha* GetLightConstant(int index) {
//	return reinterpret_cast<NiColorAlpha*>(GetConstant(index));
//}
//
//__forceinline void ScaleColor(NiColorAlpha* Color, float scale) {
//	Color->r *= scale;
//	Color->g *= scale;
//	Color->b *= scale;
//}
//
//void __fastcall ShadowLightShader__UpdateLights(void* apThis, void*, void* apShaderProp, void* apRenderPass, D3DXMATRIX aMatrix, void* apTransform, UInt32 aeRenderPassType, void* apSkinInstance) {
//	ThisCall(0xB78A90, apThis, apShaderProp, apRenderPass, aMatrix, apTransform, aeRenderPassType, apSkinInstance);
	//Logger::Log("scaling light by %f", TheShaderManager->ShaderConst.HDR.PointLightMult);
	//NiColorAlpha* pColor;

	// ambient light is constant 0
	//ScaleColor(GetLightConstant(0), TheShaderManager->ShaderConst.HDR.PointLightMult);

	// pointlight registers go from 0 to 10
	//for (UInt32 i = 0; i < 12; i++) {
	//	ScaleColor(GetLightConstant(i), TheShaderManager->ShaderConst.HDR.PointLightMult);
	//}

	// emittance color is index 27
	//ScaleColor(GetLightConstant(27), TheShaderManager->ShaderConst.HDR.PointLightMult);
//}

