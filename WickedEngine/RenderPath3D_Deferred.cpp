#include "RenderPath3D_Deferred.h"
#include "wiRenderer.h"
#include "wiImage.h"
#include "wiHelper.h"
#include "wiTextureHelper.h"
#include "wiSprite.h"
#include "ResourceMapping.h"
#include "wiProfiler.h"

using namespace wiGraphicsTypes;

wiRenderTarget RenderPath3D_Deferred::rtGBuffer, RenderPath3D_Deferred::rtDeferred, RenderPath3D_Deferred::rtLight, RenderPath3D_Deferred::rtSSS[2];

RenderPath3D_Deferred::RenderPath3D_Deferred()
{
	RenderPath3D::setProperties();
}
RenderPath3D_Deferred::~RenderPath3D_Deferred()
{
}


void RenderPath3D_Deferred::ResizeBuffers()
{
	RenderPath3D::ResizeBuffers();

	FORMAT defaultTextureFormat = wiRenderer::GetDevice()->GetBackBufferFormat();

	// Protect against multiple buffer resizes when there is no change!
	static UINT lastBufferResWidth = 0, lastBufferResHeight = 0, lastBufferMSAA = 0;
	static FORMAT lastBufferFormat = FORMAT_UNKNOWN;
	if (lastBufferResWidth == wiRenderer::GetInternalResolution().x &&
		lastBufferResHeight == wiRenderer::GetInternalResolution().y &&
		lastBufferMSAA == getMSAASampleCount() &&
		lastBufferFormat == defaultTextureFormat)
	{
		return;
	}
	else
	{
		lastBufferResWidth = wiRenderer::GetInternalResolution().x;
		lastBufferResHeight = wiRenderer::GetInternalResolution().y;
		lastBufferMSAA = getMSAASampleCount();
		lastBufferFormat = defaultTextureFormat;
	}

	rtGBuffer.Initialize(
		wiRenderer::GetInternalResolution().x, wiRenderer::GetInternalResolution().y
		, true, wiRenderer::RTFormat_gbuffer_0);
	rtGBuffer.Add(wiRenderer::RTFormat_gbuffer_1);
	rtGBuffer.Add(wiRenderer::RTFormat_gbuffer_2);
	rtGBuffer.Add(wiRenderer::RTFormat_gbuffer_3);

	rtDeferred.Initialize(
		wiRenderer::GetInternalResolution().x, wiRenderer::GetInternalResolution().y
		, false, wiRenderer::RTFormat_hdr);
	rtLight.Initialize(
		wiRenderer::GetInternalResolution().x, wiRenderer::GetInternalResolution().y
		, false, wiRenderer::RTFormat_deferred_lightbuffer); // diffuse
	rtLight.Add(wiRenderer::RTFormat_deferred_lightbuffer); // specular

	rtSSS[0].Initialize(
		wiRenderer::GetInternalResolution().x, wiRenderer::GetInternalResolution().y
		, false, wiRenderer::RTFormat_hdr);
	rtSSS[1].Initialize(
		wiRenderer::GetInternalResolution().x, wiRenderer::GetInternalResolution().y
		, false, wiRenderer::RTFormat_hdr);
}

void RenderPath3D_Deferred::Initialize()
{
	ResizeBuffers();

	RenderPath3D::Initialize();
}
void RenderPath3D_Deferred::Load()
{
	RenderPath3D::Load();
}
void RenderPath3D_Deferred::Start()
{
	RenderPath3D::Start();
}
void RenderPath3D_Deferred::Render()
{
	RenderFrameSetUp(GRAPHICSTHREAD_IMMEDIATE);
	RenderShadows(GRAPHICSTHREAD_IMMEDIATE);
	RenderReflections(GRAPHICSTHREAD_IMMEDIATE);
	RenderScene(GRAPHICSTHREAD_IMMEDIATE);
	RenderSecondaryScene(rtGBuffer, GetFinalRT(), GRAPHICSTHREAD_IMMEDIATE);
	RenderComposition(GetFinalRT(), rtGBuffer, GRAPHICSTHREAD_IMMEDIATE);

	RenderPath2D::Render();
}


void RenderPath3D_Deferred::RenderScene(GRAPHICSTHREAD threadID)
{
	wiProfiler::BeginRange("Opaque Scene", wiProfiler::DOMAIN_GPU, threadID);

	wiRenderer::UpdateCameraCB(wiRenderer::GetCamera(), threadID);

	wiImageParams fx((float)wiRenderer::GetInternalResolution().x, (float)wiRenderer::GetInternalResolution().y);

	GPUResource* dsv[] = { rtGBuffer.depth->GetTexture() };
	wiRenderer::GetDevice()->TransitionBarrier(dsv, ARRAYSIZE(dsv), RESOURCE_STATE_DEPTH_READ, RESOURCE_STATE_DEPTH_WRITE, threadID);

	rtGBuffer.Activate(threadID, 0, 0, 0, 0);
	{
		wiRenderer::GetDevice()->BindResource(PS, getReflectionsEnabled() ? rtReflection.GetTexture() : wiTextureHelper::getTransparent(), TEXSLOT_RENDERABLECOMPONENT_REFLECTION, threadID);
		wiRenderer::DrawScene(wiRenderer::GetCamera(), getTessellationEnabled(), threadID, SHADERTYPE_DEFERRED, getHairParticlesEnabled(), true, getLayerMask());
	}

	wiRenderer::GetDevice()->TransitionBarrier(dsv, ARRAYSIZE(dsv), RESOURCE_STATE_DEPTH_WRITE, RESOURCE_STATE_COPY_SOURCE, threadID);

	rtLinearDepth.Activate(threadID); {
		fx.blendFlag = BLENDMODE_OPAQUE;
		fx.sampleFlag = SAMPLEMODE_CLAMP;
		fx.quality = QUALITY_NEAREST;
		fx.process.setLinDepth();
		wiImage::Draw(rtGBuffer.depth->GetTexture(), fx, threadID);
		fx.process.clear();
	}
	rtLinearDepth.Deactivate(threadID);
	dtDepthCopy.CopyFrom(*rtGBuffer.depth, threadID);

	wiRenderer::GetDevice()->TransitionBarrier(dsv, ARRAYSIZE(dsv), RESOURCE_STATE_COPY_SOURCE, RESOURCE_STATE_DEPTH_READ, threadID);

	wiRenderer::GetDevice()->UnbindResources(TEXSLOT_ONDEMAND0, TEXSLOT_ONDEMAND_COUNT, threadID);

	wiRenderer::BindDepthTextures(dtDepthCopy.GetTexture(), rtLinearDepth.GetTexture(), threadID);

	if (getStereogramEnabled())
	{
		// We don't need the following for stereograms...
		return;
	}


	rtGBuffer.Set(threadID); {
		wiRenderer::DrawDecals(wiRenderer::GetCamera(), threadID);
	}
	rtGBuffer.Deactivate(threadID);

	wiRenderer::BindGBufferTextures(rtGBuffer.GetTexture(0), rtGBuffer.GetTexture(1), rtGBuffer.GetTexture(2), rtGBuffer.GetTexture(3), nullptr, threadID);



	rtLight.Activate(threadID, rtGBuffer.depth); {
		wiRenderer::GetDevice()->BindResource(PS, getSSREnabled() ? rtSSR.GetTexture() : wiTextureHelper::getTransparent(), TEXSLOT_RENDERABLECOMPONENT_SSR, threadID);
		wiRenderer::DrawLights(wiRenderer::GetCamera(), threadID);
	}


	if (getSSAOEnabled()) {
		wiRenderer::GetDevice()->UnbindResources(TEXSLOT_RENDERABLECOMPONENT_SSAO, 1, threadID);
		wiRenderer::GetDevice()->EventBegin("SSAO", threadID);
		fx.stencilRef = STENCILREF_DEFAULT;
		fx.stencilComp = STENCILMODE_LESS;
		rtSSAO[0].Activate(threadID); {
			fx.process.setSSAO();
			fx.setMaskMap(wiTextureHelper::getRandom64x64());
			fx.quality = QUALITY_BILINEAR;
			fx.sampleFlag = SAMPLEMODE_MIRROR;
			wiImage::Draw(nullptr, fx, threadID);
			fx.process.clear();
		}
		rtSSAO[1].Activate(threadID); {
			fx.process.setBlur(XMFLOAT2(getSSAOBlur(), 0));
			fx.blendFlag = BLENDMODE_OPAQUE;
			wiImage::Draw(rtSSAO[0].GetTexture(), fx, threadID);
		}
		rtSSAO[2].Activate(threadID); {
			fx.process.setBlur(XMFLOAT2(0, getSSAOBlur()));
			fx.blendFlag = BLENDMODE_OPAQUE;
			wiImage::Draw(rtSSAO[1].GetTexture(), fx, threadID);
			fx.process.clear();
		}
		fx.stencilRef = 0;
		fx.stencilComp = STENCILMODE_DISABLED;
		wiRenderer::GetDevice()->EventEnd(threadID);
	}

	if (getSSSEnabled())
	{
		wiRenderer::GetDevice()->EventBegin("SSS", threadID);
		fx.stencilRef = STENCILREF_SKIN;
		fx.stencilComp = STENCILMODE_LESS;
		fx.quality = QUALITY_BILINEAR;
		fx.sampleFlag = SAMPLEMODE_CLAMP;
		rtSSS[1].Activate(threadID, 0, 0, 0, 0);
		rtSSS[0].Activate(threadID, 0, 0, 0, 0);
		static int sssPassCount = 6;
		for (int i = 0; i < sssPassCount; ++i)
		{
			wiRenderer::GetDevice()->UnbindResources(TEXSLOT_ONDEMAND0, 1, threadID);
			rtSSS[i % 2].Set(threadID, rtGBuffer.depth);
			XMFLOAT2 dir = XMFLOAT2(0, 0);
			static float stren = 0.018f;
			if (i % 2 == 0)
			{
				dir.x = stren*((float)wiRenderer::GetInternalResolution().y / (float)wiRenderer::GetInternalResolution().x);
			}
			else
			{
				dir.y = stren;
			}
			fx.process.setSSSS(dir);
			if (i == 0)
			{
				wiImage::Draw(rtLight.GetTexture(0), fx, threadID);
			}
			else
			{
				wiImage::Draw(rtSSS[(i + 1) % 2].GetTexture(), fx, threadID);
			}
		}
		fx.process.clear();
		wiRenderer::GetDevice()->UnbindResources(TEXSLOT_ONDEMAND0, 1, threadID);
		rtSSS[0].Activate(threadID, rtGBuffer.depth); {
			fx.setMaskMap(nullptr);
			fx.quality = QUALITY_NEAREST;
			fx.sampleFlag = SAMPLEMODE_CLAMP;
			fx.blendFlag = BLENDMODE_OPAQUE;
			fx.stencilRef = 0;
			fx.stencilComp = STENCILMODE_DISABLED;
			fx.enableFullScreen();
			fx.enableHDR();
			wiImage::Draw(rtLight.GetTexture(0), fx, threadID);
			fx.stencilRef = STENCILREF_SKIN;
			fx.stencilComp = STENCILMODE_LESS;
			wiImage::Draw(rtSSS[1].GetTexture(), fx, threadID);
		}

		fx.stencilRef = 0;
		fx.stencilComp = STENCILMODE_DISABLED;
		wiRenderer::GetDevice()->EventEnd(threadID);
	}

	rtDeferred.Activate(threadID, rtGBuffer.depth); {
		wiImage::DrawDeferred((getSSSEnabled() ? rtSSS[0].GetTexture(0) : rtLight.GetTexture(0)), rtLight.GetTexture(1)
			, getSSAOEnabled() ? rtSSAO.back().GetTexture() : wiTextureHelper::getWhite()
			, threadID, STENCILREF_DEFAULT);
		wiRenderer::DrawSky(threadID);
	}

	if (getSSREnabled()) {
		wiRenderer::GetDevice()->UnbindResources(TEXSLOT_RENDERABLECOMPONENT_SSR, 1, threadID);
		wiRenderer::GetDevice()->EventBegin("SSR", threadID);
		rtSSR.Activate(threadID); {
			fx.process.clear();
			fx.disableFullScreen();
			fx.process.setSSR();
			fx.setMaskMap(nullptr);
			wiImage::Draw(rtDeferred.GetTexture(), fx, threadID);
			fx.process.clear();
		}
		wiRenderer::GetDevice()->EventEnd(threadID);
	}

	wiProfiler::EndRange(threadID); // Opaque Scene
}

wiRenderTarget& RenderPath3D_Deferred::GetFinalRT()
{
	//if (getSSREnabled())
	//	return rtSSR;
	//else
		return rtDeferred;
}

wiDepthTarget* RenderPath3D_Deferred::GetDepthBuffer()
{
	return rtGBuffer.depth;
}
