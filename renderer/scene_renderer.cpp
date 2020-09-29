/* Copyright (c) 2017-2020 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "scene_renderer.hpp"
#include "threaded_scene.hpp"

namespace Granite
{
void RenderPassSceneRenderer::init(const Setup &setup)
{
	setup_data = setup;
}

static Renderer::RendererOptionFlags convert_pcf_flags(SceneRendererFlags flags)
{
	if (flags & SCENE_RENDERER_SHADOW_PCF_3X_BIT)
		return Renderer::SHADOW_PCF_KERNEL_WIDTH_3_BIT;
	else if (flags & SCENE_RENDERER_SHADOW_PCF_5X_BIT)
		return Renderer::SHADOW_PCF_KERNEL_WIDTH_5_BIT;
	else
		return 0;
}

static void compose_parallel_push_renderables(TaskComposer &composer, const RenderContext &context, RenderQueue *queues, VisibilityList *visibility, unsigned count)
{
	{
		composer.begin_pipeline_stage();
		auto &group = composer.get_group();
		for (unsigned i = 0; i < count; i++)
		{
			group.enqueue_task([i, &context, visibility, queues]() {
				queues[i].push_renderables(context, visibility[i]);
			});
		}
	}

	{
		composer.begin_pipeline_stage();
		auto &group = composer.get_group();
		group.enqueue_task([=]() {
			for (unsigned i = 1; i < count; i++)
				queues[0].combine_render_info(queues[i]);
			queues[0].sort();
		});
	}
}

void RenderPassSceneRenderer::enqueue_prepare_render_pass(TaskComposer &composer, const Vulkan::RenderPassInfo &,
                                                          unsigned, VkSubpassContents &contents)
{
	contents = VK_SUBPASS_CONTENTS_INLINE;
	for (auto &visible : visible_per_task)
		visible.clear();

	// Setup renderer options in main thread.
	if (setup_data.flags & (SCENE_RENDERER_FORWARD_Z_PREPASS_BIT | SCENE_RENDERER_DEPTH_BIT))
	{
#if 0
		if (setup_data.flags & SCENE_RENDERER_DEPTH_BIT)
			setup_data.depth->set_mesh_renderer_options((setup_data.flags & SCENE_RENDERER_DEPTH_VSM_BIT) ? Renderer::SHADOW_VSM_BIT : 0);
#endif

		for (auto &queue : queue_per_task_depth)
			setup_data.depth->begin(queue);
	}

	if (setup_data.flags & SCENE_RENDERER_FORWARD_OPAQUE_BIT)
	{
#if 0
		setup_data.forward->set_mesh_renderer_options_from_lighting(*setup_data.context->get_lighting_parameters());
			setup_data.forward->set_mesh_renderer_options(
					setup_data.forward->get_mesh_renderer_options() |
					convert_pcf_flags(setup_data.flags) |
					((setup_data.flags & SCENE_RENDERER_FORWARD_Z_PREPASS_BIT) ? Renderer::ALPHA_TEST_DISABLE_BIT : 0));
#endif
		for (auto &queue : queue_per_task_opaque)
			setup_data.forward_opaque->begin(queue);
	}
	else if (setup_data.flags & SCENE_RENDERER_DEFERRED_GBUFFER_BIT)
	{
		for (auto &queue : queue_per_task_opaque)
			setup_data.deferred->begin(queue);
	}

	if (setup_data.flags & SCENE_RENDERER_FORWARD_TRANSPARENT_BIT)
	{
#if 0
		setup_data.forward->set_mesh_renderer_options_from_lighting(*setup_data.context->get_lighting_parameters());
			setup_data.forward->set_mesh_renderer_options(
					setup_data.forward->get_mesh_renderer_options() |
					convert_pcf_flags(setup_data.flags));
#endif
		for (auto &queue : queue_per_task_transparent)
			setup_data.forward_transparent->begin(queue);
	}

	if (setup_data.flags & (SCENE_RENDERER_FORWARD_OPAQUE_BIT | SCENE_RENDERER_FORWARD_Z_PREPASS_BIT))
	{
		setup_data.scene->gather_visible_render_pass_sinks(setup_data.context->get_render_parameters().camera_position, visible_per_task[0]);
		Threaded::scene_gather_opaque_renderables(*setup_data.scene, composer, setup_data.context->get_visibility_frustum(), visible_per_task, MaxTasks);

		if (setup_data.flags & SCENE_RENDERER_FORWARD_Z_PREPASS_BIT)
			compose_parallel_push_renderables(composer, *setup_data.context, queue_per_task_depth, visible_per_task, MaxTasks);

		if (setup_data.flags & SCENE_RENDERER_FORWARD_OPAQUE_BIT)
		{
			setup_data.scene->gather_unbounded_renderables(visible_per_task[0]);
			compose_parallel_push_renderables(composer, *setup_data.context, queue_per_task_opaque, visible_per_task, MaxTasks);
		}
	}

	if (setup_data.flags & SCENE_RENDERER_DEFERRED_GBUFFER_BIT)
	{
		setup_data.scene->gather_visible_render_pass_sinks(setup_data.context->get_render_parameters().camera_position, visible_per_task[0]);
		setup_data.scene->gather_unbounded_renderables(visible_per_task[0]);
		Threaded::scene_gather_opaque_renderables(*setup_data.scene, composer, setup_data.context->get_visibility_frustum(), visible_per_task, MaxTasks);
		compose_parallel_push_renderables(composer, *setup_data.context, queue_per_task_opaque, visible_per_task, MaxTasks);
	}

	if (setup_data.flags & SCENE_RENDERER_FORWARD_TRANSPARENT_BIT)
	{
		Threaded::scene_gather_transparent_renderables(*setup_data.scene, composer, setup_data.context->get_visibility_frustum(), visible_per_task_transparent, MaxTasks);
		compose_parallel_push_renderables(composer, *setup_data.context, queue_per_task_transparent, visible_per_task_transparent, MaxTasks);
	}

	if (setup_data.flags & SCENE_RENDERER_DEPTH_BIT)
	{
		if (setup_data.flags & SCENE_RENDERER_DEPTH_DYNAMIC_BIT)
			Threaded::scene_gather_dynamic_shadow_renderables(*setup_data.scene, composer, setup_data.context->get_visibility_frustum(), visible_per_task, MaxTasks);
		if (setup_data.flags & SCENE_RENDERER_DEPTH_STATIC_BIT)
			Threaded::scene_gather_static_shadow_renderables(*setup_data.scene, composer, setup_data.context->get_visibility_frustum(), visible_per_task, MaxTasks);
		compose_parallel_push_renderables(composer, *setup_data.context, queue_per_task_opaque, visible_per_task, MaxTasks);
	}
}

void RenderPassSceneRenderer::build_render_pass(Vulkan::CommandBuffer &cmd)
{
	if (setup_data.flags & (SCENE_RENDERER_FORWARD_OPAQUE_BIT | SCENE_RENDERER_FORWARD_Z_PREPASS_BIT))
	{
		if (setup_data.flags & SCENE_RENDERER_FORWARD_Z_PREPASS_BIT)
		{
			setup_data.depth->flush(cmd, queue_per_task_depth[0], *setup_data.context,
			                        Renderer::NO_COLOR_BIT | Renderer::SKIP_SORTING_BIT);
		}

		if (setup_data.flags & SCENE_RENDERER_FORWARD_OPAQUE_BIT)
		{
			Renderer::RendererOptionFlags opt = Renderer::SKIP_SORTING_BIT;
			if (setup_data.flags & SCENE_RENDERER_FORWARD_Z_PREPASS_BIT)
				opt |= Renderer::DEPTH_STENCIL_READ_ONLY_BIT | Renderer::DEPTH_TEST_EQUAL_BIT;
			setup_data.forward_opaque->flush(cmd, queue_per_task_opaque[0], *setup_data.context, opt);
		}
	}

	if (setup_data.flags & SCENE_RENDERER_DEFERRED_GBUFFER_BIT)
		setup_data.deferred->flush(cmd, queue_per_task_opaque[0], *setup_data.context, Renderer::SKIP_SORTING_BIT);

	if (setup_data.flags & SCENE_RENDERER_DEFERRED_GBUFFER_LIGHT_PREPASS_BIT)
		setup_data.deferred_lights->render_prepass_lights(cmd, queue_non_tasked, *setup_data.context);

	if (setup_data.flags & SCENE_RENDERER_DEFERRED_LIGHTING_BIT)
	{
		if (!(setup_data.flags & SCENE_RENDERER_DEFERRED_CLUSTER_BIT))
			setup_data.deferred_lights->render_lights(cmd, queue_non_tasked, *setup_data.context, convert_pcf_flags(setup_data.flags));
		DeferredLightRenderer::render_light(cmd, *setup_data.context, convert_pcf_flags(setup_data.flags));
	}

	if (setup_data.flags & SCENE_RENDERER_FORWARD_TRANSPARENT_BIT)
	{
		setup_data.forward_transparent->flush(cmd, queue_per_task_transparent[0], *setup_data.context,
		                                      Renderer::DEPTH_STENCIL_READ_ONLY_BIT | Renderer::SKIP_SORTING_BIT);
	}

	if (setup_data.flags & SCENE_RENDERER_DEPTH_BIT)
	{
		setup_data.depth->flush(cmd, queue_per_task_depth[0], *setup_data.context,
		                        Renderer::DEPTH_BIAS_BIT | Renderer::SKIP_SORTING_BIT);
	}
}

void RenderPassSceneRenderer::set_clear_color(const VkClearColorValue &value)
{
	clear_color_value = value;
}

bool RenderPassSceneRenderer::get_clear_color(unsigned, VkClearColorValue *value) const
{
	if (value)
		*value = clear_color_value;
	return true;
}

bool RenderPassSceneRenderer::render_pass_can_multithread() const
{
	return true;
}

void RenderPassSceneRendererConditional::set_need_render_pass(bool need_)
{
	need = need_;
}

bool RenderPassSceneRendererConditional::need_render_pass() const
{
	return need;
}

bool RenderPassSceneRendererConditional::render_pass_is_conditional() const
{
	return true;
}
}
