////////////////////////////////////////////////////////////////////////////////////////////////////
// Noesis Engine - http://www.noesisengine.com
// Copyright (c) 2009-2010 Noesis Technologies S.L. All Rights Reserved.
////////////////////////////////////////////////////////////////////////////////////////////////////


#include <OgreNsGuiBindings.h>
#include <NsGui/IRenderer.h>
#include <NsGui/IUIResource.h>
#include <NsGui/FrameworkElement.h>
#include <NsGui/ResourceDictionary.h>
#include <NsGui/Collection.h>
#include <NsGui/VisualTreeHelper.h>
#include <NsCore/Kernel.h>
#include <NsCore/NsSystem.h>
#include <NsCore/IKernelSystem.h>
#include <NsCore/StringUtils.h>
#include <NsCore/LoggerMacros.h>
#include <NsCore/Dll.h>
#include <NsCore/Vector.h>
#include <NsDrawing/IVGLSystem.h>
#include <NsDrawing/IVGLSurface.h>
#include <NsRender/IRenderTarget2D.h>
#include <NsResource/IResourceSystem.h>
#include <NsResource/BaseResource.h>
#include <NsRender/IDX9RenderSystem.h>
#include <NsCore/Symbol.h>

#include "OgreNsGuiFileSystem.h"
#include "OgreRoot.h"
#include "OgreRenderSystem.h"
#include "OgreRenderWindow.h"
#include "OgreSceneManager.h"
#include "OgreRenderQueueListener.h"

#include <d3d9.h>


#define NS_LOG_OGRE(...) NS_LOG(LogSeverity_Info, "OGRE", __VA_ARGS__)


using namespace Noesis;
using namespace Noesis::Core;
using namespace Noesis::Gui;
using namespace Noesis::Drawing;
using namespace Noesis::Resource;
using namespace Noesis::Render;
using namespace Noesis::File;

namespace
{
	////////////////////////////////////////////////////////////////////////////////////////////////////
	IDirect3DDevice9* gD3D9Device = 0;
	IDirect3DStateBlock9* gStateBlock = 0;
	IDirect3DSurface9* gRenderTarget = 0;
	IDirect3DSurface9* gDepthBuffer = 0;
	Drawing::Recti gViewport;

	class OgreRenderer;
	OgreRenderer* gOgreRender = 0;

	NsVector<Ptr<IRenderer> > gRenderers;

	struct RendererInfo
	{
		Ptr<IVGLSurface> surface;
		Ptr<RenderCommands> commands;
	};
	NsMap<Ptr<IRenderer>, RendererInfo> gRenderersInfo;

	////////////////////////////////////////////////////////////////////////////////////////////////////
	Ptr<IVGLSurface> CreateVGLSurface()
	{
		const Ptr<IDX9RenderSystem>& renderSystem = NsGetSystem<IDX9RenderSystem>();

		Ptr<IRenderTarget2D> target = renderSystem->WrapRenderTarget(gRenderTarget, gDepthBuffer);

		return NsGetSystem<IVGLSystem>()->CreateSurface(target.GetPtr());
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
	void ReleaseRenderTargets(IDirect3DSurface9*& renderTarget, IDirect3DSurface9*& depthBuffer)
	{
		if (renderTarget != 0)
		{
			renderTarget->Release();
			renderTarget = 0;
		}

		if (depthBuffer != 0)
		{
			depthBuffer->Release();
			depthBuffer = 0;
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
	NsBool UpdateRenderTargets()
	{
		NsBool changed = false;

		IDirect3DSurface9* renderTarget;
		gD3D9Device->GetRenderTarget(0, &renderTarget);
		IDirect3DSurface9* depthBuffer;
		gD3D9Device->GetDepthStencilSurface(&depthBuffer);

		if (renderTarget != 0 && depthBuffer != 0)
		{
			// update render targets
			if (gRenderTarget != renderTarget || gDepthBuffer != depthBuffer)
			{
				changed = true;

				ReleaseRenderTargets(gRenderTarget, gDepthBuffer);

				gRenderTarget = renderTarget;
				gRenderTarget->AddRef();

				gDepthBuffer = depthBuffer;
				gDepthBuffer->AddRef();

				D3DSURFACE_DESC rt;
				gRenderTarget->GetDesc(&rt);

				NS_LOG_OGRE("RenderTarget changed: [0x%p|0x%p] %ux%u", gRenderTarget, gDepthBuffer,
					rt.Width, rt.Height);
			}
		}

		// release references
		ReleaseRenderTargets(renderTarget, depthBuffer);

		return changed;
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
	void UpdateViewport()
	{
		D3DVIEWPORT9 vp;
		gD3D9Device->GetViewport(&vp);

		// update surface viewport
		if (gViewport.x != (NsInt)vp.X || gViewport.y != (NsInt)vp.Y ||
			gViewport.width != (NsInt)vp.Width || gViewport.height != (NsInt)vp.Height)
		{
			gViewport.x = vp.X;
			gViewport.y = vp.Y;
			gViewport.width = vp.Width;
			gViewport.height = vp.Height;

			NS_LOG_OGRE("Viewport changed: (%d,%d,%d,%d)", vp.X, vp.Y, vp.Width, vp.Height);
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
	void GetD3D9Device()
	{
		Ogre::RenderWindow* pWnd = Ogre::Root::getSingleton().getAutoCreatedWindow();
		if (pWnd)
		{
			pWnd->getCustomAttribute("D3DDEVICE", &gD3D9Device);
		}
		else
		{
			NS_LOG_OGRE("No Autocreated RenderWindow found!");
		}	
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////
	// OgreRenderer
	////////////////////////////////////////////////////////////////////////////////////////////////////

	class OgreRenderer :	
		public Ogre::RenderQueueListener, 
		public Ogre::RenderSystem::Listener
	{
	public:
		OgreRenderer(Ogre::SceneManager* sceneMgr) : mSceneMgr(sceneMgr)
		{
			Ogre::Root::getSingletonPtr()->getRenderSystem()->addListener(this);

			if (mSceneMgr)
			{
				mSceneMgr->addRenderQueueListener(this);
			}
		}

		virtual ~OgreRenderer()
		{
			Ogre::Root::getSingletonPtr()->getRenderSystem()->removeListener(this);

			if (mSceneMgr)
			{
				mSceneMgr->removeRenderQueueListener(this);
			}
		}

		void renderQueueStarted(Ogre::uint8 queueGroupId, const Ogre::String& invocation, bool& skipThisInvocation)
		{
			try
			{
				// We're going to render ourselves in the Overlay Queue so we're always on top
				if(queueGroupId == Ogre::RENDER_QUEUE_OVERLAY)
				{
					if (gStateBlock != 0)
					{
						NsBool rtChanged = UpdateRenderTargets();
						UpdateViewport();

						if (gRenderTarget != 0 && gDepthBuffer != 0)
						{
							gStateBlock->Capture();

							gD3D9Device->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 1.0f, 1);
							NsGetSystem<IDX9RenderSystem>()->SyncInternalState();

							NsSize numRenderers = gRenderers.size();
							for (NsSize i = 0; i < numRenderers; ++i)
							{
								const Ptr<IRenderer>& renderer = gRenderers[i];
								RendererInfo& ri = gRenderersInfo[renderer];

								// if render target changed destroy surface to free resources
								if (rtChanged)
								{
									renderer->SetSurface(0);
									{
										ri.surface.Reset();
									}
								}

								// create a new surface if necessary
								NsBool surfaceCreated = false;
								if (!ri.surface)
								{
									Ptr<IVGLSurface> surface = CreateVGLSurface();
									{
										ri.surface = surface;
									}

									surfaceCreated = true;
									NS_LOG_OGRE("Surfaces created");
								}

								// read render commands
								if (surfaceCreated)
								{
									// commands generated using old render targets can't be used anymore
									renderer->WaitForUpdate();
									{
										ri.commands.Reset();
									}
								}
								else
								{
									// get render commands generated in the update thread
									Ptr<RenderCommands> commands;
									{
										commands = ri.commands;
									}

									if (commands)
									{
										renderer->Render(commands.GetPtr());
										renderer->WaitForRender();
									}
								}
							}

							gStateBlock->Apply();
						}
					}
					else
					{
						deviceResetCallback();
					}
				}
			}
			catch (Ogre::RenderingAPIException* e)
			{
				NS_LOG_OGRE(e->getDescription());
			}
		}

		void eventOccurred(const Ogre::String& eventName, const Ogre::NameValuePairList* parameters)
		{
			if (eventName == "DeviceLost")
			{
				deviceLostCallback();
			}
			else if(eventName == "DeviceRestored")
			{
				deviceResetCallback();
			}
		}

	private:
		Ogre::SceneManager* mSceneMgr;

		void deviceLostCallback()
		{
			NS_LOG_OGRE("Device Lost");

			if (gD3D9Device != 0)
			{
				if (gStateBlock != 0)
				{
					gStateBlock->Release();
					gStateBlock = 0;
				}

				NsGetSystem<IDX9RenderSystem>()->OnLostDevice();

				ReleaseRenderTargets(gRenderTarget, gDepthBuffer);
			}
		}

		void deviceResetCallback()
		{
			NS_LOG_OGRE("Device Reset");

			if (gD3D9Device != 0)
			{
				NsGetSystem<IDX9RenderSystem>()->OnResetDevice();

				gD3D9Device->CreateStateBlock(D3DSBT_ALL, &gStateBlock);
			}
		}
	};
}

////////////////////////////////////////////////////////////////////////////////////////////////////
void NoesisErrorHandler(const NsChar* filename, NsInt line, const NsChar* desc)
{
	printf("\nERROR: %s\n\n", desc);
	exit(1);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_Init(Ogre::SceneManager* sceneMgr)
{
	GetD3D9Device();

	if (gD3D9Device != 0)
	{
		Noesis::Core::SetErrorHandler(NoesisErrorHandler);

		NsGetKernel()->Init();
		IResourceSystem::SetFileSystem(new OgreNsGuiFileSystem());
		// Configure systems before their initialization
		//@{

		gD3D9Device->CreateStateBlock(D3DSBT_ALL, &gStateBlock);
		IDX9RenderSystem::SetDevice(gD3D9Device);
		//@}

		NsGetKernel()->InitSystems();
		gOgreRender = new OgreRenderer(sceneMgr);
	}
	else
	{
		NS_ERROR("Ogre3D Graphics Device not hooked");
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_Shutdown()
{
	if (gD3D9Device != 0)
	{
		delete gOgreRender;
		gOgreRender = 0;
		{
			gRenderers.clear();
			gRenderersInfo.clear();
		}

		NsGetKernel()->Shutdown();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_Tick()
{
	if (gD3D9Device != 0)
	{
		NsGetKernel()->Tick();
	}
}
NS_DECLARE_SYMBOL(ResourceSystem) 
////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_LoadXAML(void** root, void** uiRenderer, const char* xamlFile,
	const char* resourcesFile)
{
	if (gD3D9Device != 0)
	{
		Ptr<IUIResource> resource;
		auto resourceSystem = NsGetSystem<IResourceSystem>();

		resource = NsDynamicCast<Ptr<IUIResource> >(resourceSystem->Load(xamlFile));
		Ptr<FrameworkElement> element(NsDynamicCast<FrameworkElement*>(resource->GetRoot()));
		if (element)
		{
			if (!String::IsNullOrEmpty(resourcesFile))
			{
				resource = NsDynamicCast<Ptr<IUIResource> >(resourceSystem->Load(resourcesFile));
				Ptr<ResourceDictionary> resources(NsDynamicCast<ResourceDictionary*>(
					resource->GetRoot()));
				if (resources)
				{
					element->GetResources()->GetMergedDictionaries()->Add(resources.GetPtr());
				}
			}

			Ptr<IRenderer> renderer = Gui::CreateRenderer(element.GetPtr());
			{
				gRenderers.push_back(renderer);
				gRenderersInfo.insert(nstl::make_pair_ref(renderer, RendererInfo()));
			}

			*root = element.GetPtr();
			*uiRenderer = renderer.GetPtr();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_RendererClearMode(void* uiRenderer, int mode)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->SetSurfaceClearMode((SurfaceClearMode)mode);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_RendererAntialiasingMode(void* uiRenderer, int mode)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->SetAntialiasingMode((AntialiasingMode)mode);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_RendererTessMode(void* uiRenderer, int mode)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->SetTessellationMode((TessellationMode)mode);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_RendererTessQuality(void* uiRenderer, int quality)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->SetTessellationQuality((TessellationQuality)quality);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_RendererDebugFlags(void* uiRenderer, int flags)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->SetDebugFlags((NsUInt32)flags);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_UpdateRenderer(void* uiRenderer, double time)
{
	Ptr<IRenderer> renderer(static_cast<IRenderer*>(uiRenderer));
	RendererInfo& ri = gRenderersInfo[renderer];

	Ptr<IVGLSurface> surface;
	Recti viewport;
	{
		surface = ri.surface;
		viewport = gViewport;
	}
	
	if (surface)
	{
		renderer->SetSurface(surface.GetPtr(), &viewport);
		renderer->Update(time);

		{
			ri.commands.Reset(renderer->WaitForUpdate());
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT bool Noesis_HitTest(void* root, float x, float y)
{
	Visual* visual = static_cast<Visual*>(root);
	Drawing::Point point(x, y);

	return VisualTreeHelper::HitTest(visual, point).visualHit != 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_MouseButtonDown(void* uiRenderer, float x, float y,
	int button)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->MouseButtonDown((NsInt)x, (NsInt)y, static_cast<MouseButton>(button));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_MouseButtonUp(void* uiRenderer, float x, float y, int button)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->MouseButtonUp((NsInt)x, (NsInt)y, static_cast<MouseButton>(button));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_MouseDoubleClick(void* uiRenderer, float x, float y,
	int button)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->MouseDoubleClick((NsInt)x, (NsInt)y, static_cast<MouseButton>(button));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_MouseMove(void* uiRenderer, float x, float y)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->MouseMove((NsInt)x, (NsInt)y);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_MouseWheel(void* uiRenderer, float x, float y,
	int wheelRotation)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->MouseWheel((NsInt)x, (NsInt)y, wheelRotation);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_KeyDown(void* uiRenderer, int key)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->KeyDown(static_cast<Key>(key));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_KeyUp(void* uiRenderer, int key)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->KeyUp(static_cast<Key>(key));
}

////////////////////////////////////////////////////////////////////////////////////////////////////
extern "C" NS_DLL_EXPORT void Noesis_Char(void* uiRenderer, wchar_t ch)
{
	IRenderer* renderer = static_cast<IRenderer*>(uiRenderer);
	renderer->Char(ch);
}
