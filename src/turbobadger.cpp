#include "turbobadger.h"
#include "engine/crc32.h"
#include "engine/delegate.h"
#include "engine/delegate_list.h"
#include "engine/engine.h"
#include "engine/iallocator.h"
#include "engine/input_system.h"
#include "engine/lua_wrapper.h"
#include "engine/matrix.h"
#include "engine/path.h"
#include "engine/plugin_manager.h"
#include "engine/resource_manager.h"
#include "renderer/material.h"
#include "renderer/material_manager.h"
#include "renderer/pipeline.h"
#include "renderer/shader.h"
#include "renderer/renderer.h"

#pragma warning(disable : 4267)
#include <animation/tb_animation.h>
#include <bgfx/bgfx.h>
#include <tb_core.h>
#include <tb_font_renderer.h>
#include <tb_language.h>
#include <tb_msg.h>
#include <tb_node_tree.h>
#include <tb_renderer.h>
#include <renderers/tb_renderer_batcher.h>
#include <tb_system.h>
#include <tb_widgets.h>
#include <tb_widgets_common.h>
#include <tb_widgets_reader.h>


using namespace tb;


namespace tb
{

void TBSystem::RescheduleTimer(double fire_time) {}

}


namespace Lumix
{


static const ResourceType MATERIAL_TYPE("material");



struct BGFXBitmap : public TBBitmap
{
	int Width() override { return w; }
	int Height() override { return h; }


	void SetData(uint32* data) override
	{
		pipeline->destroyTexture(tex);
		tex = pipeline->createTexture(w, h, data);
	}

	Pipeline* pipeline;
	bgfx::TextureHandle tex;
	int w, h;
};


struct GUIRenderer : public TBRendererBatcher
{
	GUIRenderer(Engine& engine)
		: m_pipeline(nullptr)
	{
		m_decl.begin()
			.add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
			.end();
		auto* material_manager = engine.getResourceManager().get(MATERIAL_TYPE);
		auto* resource = material_manager->load(Path("pipelines/imgui/imgui.mat"));
		m_material = static_cast<Material*>(resource);
	}


	~GUIRenderer()
	{
		m_material->getResourceManager().unload(*m_material);

		if (m_pipeline)	m_pipeline->destroyUniform(m_texture_uniform);
	}


	TBBitmap* CreateBitmap(int width, int height, uint32* data) override
	{
		if (!m_pipeline) return nullptr;

		auto* bitmap = new BGFXBitmap();
		bitmap->tex = m_pipeline->createTexture(width, height, data);
		bitmap->w = width;
		bitmap->h = height;
		bitmap->pipeline = m_pipeline;
		return bitmap;
	}


	void RenderBatch(Batch* batch) override
	{
		if (!m_material->isReady()) return;
		if (!m_pipeline->checkAvailTransientBuffers(batch->vertex_count, m_decl, batch->vertex_count)) return;
		bgfx::TransientVertexBuffer vertex_buffer;
		bgfx::TransientIndexBuffer index_buffer;
		m_pipeline->allocTransientBuffers(
			&vertex_buffer, batch->vertex_count, m_decl, &index_buffer, batch->vertex_count);

		int16* idcs = (int16*)index_buffer.data;
		struct Vertex
		{
			float x, y;
			float u, v;
			uint32 color;
		};
		Vertex* vtcs = (Vertex*)vertex_buffer.data;
		for (int i = 0; i < batch->vertex_count; ++i)
		{
			idcs[i] = i;
			vtcs[i].x = batch->vertex[i].x;
			vtcs[i].y = batch->vertex[i].y;
			vtcs[i].u = batch->vertex[i].u;
			vtcs[i].v = batch->vertex[i].v;
			vtcs[i].color = batch->vertex[i].col;
		}

		if (batch->bitmap) m_pipeline->setTexture(0, ((BGFXBitmap*)(batch->bitmap))->tex, m_texture_uniform);

		m_pipeline->render(vertex_buffer,
			index_buffer,
			Matrix::IDENTITY,
			0,
			batch->vertex_count,
			BGFX_STATE_BLEND_ALPHA,
			m_material->getShaderInstance());
	}


	void SetClipRect(const TBRect& rect) override { m_pipeline->setScissor(rect.x, rect.y, rect.w, rect.h); }


	Material* m_material;
	bgfx::VertexDecl m_decl;
	Pipeline* m_pipeline;
	bgfx::UniformHandle m_texture_uniform;
};


struct TurboBadgerImpl : public TurboBadger
{
	TurboBadgerImpl(Engine& engine)
		: m_engine(engine)
		, m_renderer(engine)
	{
		tb_core_init(&m_renderer);

//		engine.getInputSystem().eventListener().bind<TurboBadgerImpl, &TurboBadgerImpl::onInputEvent>(this);
		registerLuaAPI();
	}


	~TurboBadgerImpl()
	{
//		m_engine.getInputSystem().eventListener().unbind<TurboBadgerImpl, &TurboBadgerImpl::onInputEvent>(this);
		tb_core_shutdown();
	}


	void showGUI(bool show)
	{
		m_root_widget.SetVisibility(show ? WIDGET_VISIBILITY_VISIBLE : WIDGET_VISIBILITY_GONE);
	}


	bool isGUIShown() const
	{
		return m_root_widget.GetVisibility() == WIDGET_VISIBILITY_VISIBLE;
	}


	void loadFile(const char* path)
	{
		m_root_widget.DeleteAllChildren();
		g_widgets_reader->LoadFile(&m_root_widget, path);
	}


	void registerLuaAPI()
	{
		lua_State* L = m_engine.getState();
		
		#define REGISTER_FUNCTION(name) \
			do {\
				auto f = &LuaWrapper::wrapMethod<TurboBadgerImpl, decltype(&TurboBadgerImpl::name), \
					&TurboBadgerImpl::name>; \
				LuaWrapper::createSystemFunction(L, "TurboBadger", #name, f); \
			} while(false) \

		REGISTER_FUNCTION(showGUI);
		REGISTER_FUNCTION(isGUIShown);
		REGISTER_FUNCTION(loadFile);

		LuaWrapper::createSystemVariable(L, "TurboBadger", "instance", this);

		#undef REGISTER_FUNCTION
	}


	void pipelineCallback()
	{
		Renderer* renderer = (Renderer*)m_engine.getPluginManager().getPlugin("renderer");
		Pipeline* pipeline = renderer->getMainPipeline();
		
		int w = (int)pipeline->getWidth();
		int h = (int)pipeline->getHeight();
		m_renderer.BeginPaint(w, h);

		Lumix::Matrix ortho;
		m_root_widget.SetSize(w, h);
		ortho.setOrtho(0.0f, (float)w, (float)h, 0.0f, -1.0f, 1.0f, false);
		pipeline->setViewProjection(ortho, w, h);

		m_root_widget.InvokePaint(TBWidget::PaintProps());
		m_renderer.EndPaint();
	}

	/*
	void onInputEvent(InputSystem::InputEvent& event)
	{
		Vec2 mouse_pos = m_engine.getInputSystem().getMousePos() - m_interface->getPos();
		switch (event.type)
		{
			case InputSystem::InputEvent::POINTER_DOWN:
				m_root_widget.InvokePointerDown((int)mouse_pos.x, (int)mouse_pos.y, 1, TB_MODIFIER_NONE, false);
				break;
			case InputSystem::InputEvent::POINTER_UP:
				m_root_widget.InvokePointerUp((int)mouse_pos.x, (int)mouse_pos.y, TB_MODIFIER_NONE, false);
				break;
			case InputSystem::InputEvent::POINTER_MOVE:
				m_root_widget.InvokePointerMove((int)mouse_pos.x, (int)mouse_pos.y, TB_MODIFIER_NONE, false);
				break;
			case InputSystem::InputEvent::KEY_DOWN:
				m_root_widget.InvokeKey(event.key.sym, TB_KEY_UNDEFINED, TB_MODIFIER_NONE, true);
				break;
			case InputSystem::InputEvent::KEY_UP:
				m_root_widget.InvokeKey(event.key.sym, TB_KEY_UNDEFINED, TB_MODIFIER_NONE, false);
				break;
		}
	}*/


	void update(float) override
	{
		if (!m_renderer.m_pipeline)
		{
			Renderer* renderer = (Renderer*)m_engine.getPluginManager().getPlugin("renderer");
			Pipeline* pipeline = renderer->getMainPipeline();
			if (pipeline)
			{
				m_renderer.m_pipeline = pipeline;
				pipeline->addCustomCommandHandler("renderTurbobadger").callback.bind<TurboBadgerImpl, &TurboBadgerImpl::pipelineCallback>(this);
				m_renderer.m_texture_uniform = m_renderer.m_pipeline->createTextureUniform("u_texture");

				void register_stb_font_renderer();
				register_stb_font_renderer();

				g_font_manager->AddFontInfo("gui/veramono.ttf", "Vera");
				TBFontDescription fd;
				fd.SetID(TBIDC("Vera"));
				fd.SetSize(g_tb_skin->GetDimensionConverter()->DpToPx(14));
				g_font_manager->SetDefaultFontDescription(fd);

				TBFontFace* font = g_font_manager->CreateFontFace(g_font_manager->GetDefaultFontDescription());
				if (font)
					font->RenderGlyphs(
						" !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz");

				g_tb_lng->Load("gui/language/lng_en.tb.txt");
				g_tb_skin->Load("gui/default_skin/skin.tb.txt");
			}
		}
		
		InputSystem& input_system = m_engine.getInputSystem();
		
		const InputSystem::Event* events = input_system.getEvents();
		for (int i = 0, c = input_system.getEventsCount(); i < c; ++i)
		{
			switch (events[i].type)
			{
				case InputSystem::Event::AXIS:
					if (events[i].device->type == InputSystem::Device::MOUSE)
					{
						float x = events[i].data.axis.x_abs;
						float y = events[i].data.axis.y_abs;
						m_root_widget.InvokePointerMove((int)x, (int)y, TB_MODIFIER_NONE, false);
					}
					break;
				case InputSystem::Event::BUTTON:
					if (events[i].device->type == InputSystem::Device::MOUSE)
					{
						float x = events[i].data.button.x_abs;
						float y = events[i].data.button.y_abs;
						if(events[i].data.button.state == InputSystem::ButtonEvent::DOWN)
							m_root_widget.InvokePointerDown((int)x, (int)y, 1, TB_MODIFIER_NONE, false);
						else
							m_root_widget.InvokePointerUp((int)x, (int)y, TB_MODIFIER_NONE, false);
					}
					break;
			}
		}

		TBAnimationManager::Update();
		m_root_widget.InvokeProcessStates();
		m_root_widget.InvokeProcess();
		TBMessageHandler::ProcessMessages();
	}


	const char* getName() const override { return "turbobadger"; }


	Engine& m_engine;
	TBWidget m_root_widget;
	GUIRenderer m_renderer;
};


LUMIX_PLUGIN_ENTRY(lumixengine_turbobadger)
{
	return LUMIX_NEW(engine.getAllocator(), TurboBadgerImpl)(engine);
}


} // namespace Lumix