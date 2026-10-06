#include "application.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#include <imgui.h>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace application {

namespace {

constexpr glm::vec3 cube_vertices[] = {
	{-1.0f, -1.0f, -1.0f},
	{ 1.0f, -1.0f, -1.0f},
	{ 1.0f,  1.0f, -1.0f},
	{-1.0f,  1.0f, -1.0f},
	{-1.0f, -1.0f,  1.0f},
	{ 1.0f, -1.0f,  1.0f},
	{ 1.0f,  1.0f,  1.0f},
	{-1.0f,  1.0f,  1.0f},
};


constexpr uint16_t cube_indices[] = {
	0, 2, 1, 0, 3, 2, //задняя
	4, 5, 6, 4, 6, 7, //передняя
	0, 4, 7, 0, 7, 3, //левая
	1, 2, 6, 1, 6, 5, //правая
	0, 1, 5, 0, 5, 4, //низ
	3, 7, 6, 3, 6, 2, //верх
};


struct PushConstants {
	glm::mat4 mvp;
	glm::vec4 tint;
};

VkBuffer vertex_buffer;
VmaAllocation vertex_buffer_allocation;

VkBuffer index_buffer;
VmaAllocation index_buffer_allocation;

VkPipelineLayout pipeline_layout;
VkPipeline pipeline;


glm::vec3 object_position{0.0f, 0.0f, 0.0f};
glm::vec3 object_rotation{0.0f, 0.0f, 0.0f}; //углы
glm::vec3 object_scale{1.0f, 1.0f, 1.0f};


glm::vec4 object_tint{1.0f, 1.0f, 1.0f, 1.0f};

bool anim_playing = true;
float anim_speed = 1.0f;
float anim_time = 0.0f;


bool traj_enabled = true;
float traj_radius = 1.5f;
glm::vec3 traj_freq{1.0f, 2.0f, 1.5f};
glm::vec3 traj_phase{0.0f, 0.25f, 0.0f}; //в оборотах, умножаем на 2pi


bool auto_rotate = true;
glm::vec3 rot_speed{0.0f, 45.0f, 30.0f};


int projection_mode = 0; //0 перспективное, 1 ортографическое
float fov_deg = 60.0f;
float ortho_size = 3.0f;
float near_plane = 0.1f;
float far_plane = 100.0f;

glm::vec3 camera_pos{0.0f, 0.0f, 5.0f};
float camera_yaw = 0.0f;
float camera_pitch = 0.0f;


std::vector<char> readFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		std::cerr << "Failed to open shader file: " << path << '\n';
		return {};
	}
	const std::streamsize size = f.tellg();
	std::vector<char> buffer(static_cast<size_t>(size));
	f.seekg(0);
	f.read(buffer.data(), size);
	return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const std::string& path) {
	const auto code = readFile(path);
	if (code.empty()) {
		return VK_NULL_HANDLE;
	}

	const VkShaderModuleCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = code.size(),
		.pCode = reinterpret_cast<const uint32_t*>(code.data()),
	};

	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
		std::cerr << "Failed to create shader module: " << path << '\n';
		return VK_NULL_HANDLE;
	}
	return module;
}

bool createBufferWithData(VkDeviceSize size, VkBufferUsageFlags usage,
                          const void* data, VkBuffer& out_buffer,
                          VmaAllocation& out_allocation) {
	auto& ctx = graphics::internal::context;

	const VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};

	const VmaAllocationCreateInfo alloc_info = {
		.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
		         VMA_ALLOCATION_CREATE_MAPPED_BIT,
		.usage = VMA_MEMORY_USAGE_AUTO,
	};

	VmaAllocationInfo mapped{};
	if (vmaCreateBuffer(ctx.allocator, &buffer_info, &alloc_info,
	                    &out_buffer, &out_allocation, &mapped) != VK_SUCCESS) {
		return false;
	}

	std::memcpy(mapped.pMappedData, data, size_t(size));
	return true;
}

bool createPipeline() {
	auto& ctx = graphics::internal::context;

	VkShaderModule vert = createShaderModule(ctx.device, SHADER_DIR "/cube.vert.spv");
	VkShaderModule frag = createShaderModule(ctx.device, SHADER_DIR "/cube.frag.spv");
	if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
		return false;
	}

	const VkPipelineShaderStageCreateInfo stages[] = {
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vert,
			.pName = "main",
		},
		{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag,
			.pName = "main",
		},
	};

	const VkVertexInputBindingDescription vertex_binding = {
		.binding = 0,
		.stride = sizeof(glm::vec3),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	const VkVertexInputAttributeDescription vertex_attr = {
		.location = 0,
		.binding = 0,
		.format = VK_FORMAT_R32G32B32_SFLOAT,
		.offset = 0,
	};
	const VkPipelineVertexInputStateCreateInfo vertex_input = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &vertex_binding,
		.vertexAttributeDescriptionCount = 1,
		.pVertexAttributeDescriptions = &vertex_attr,
	};

	const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};

	const VkPipelineViewportStateCreateInfo viewport_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};

	const VkPipelineRasterizationStateCreateInfo rasterizer = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_BACK_BIT,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};

	const VkPipelineMultisampleStateCreateInfo multisample = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};

	const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
		.depthTestEnable = VK_TRUE,
		.depthWriteEnable = VK_TRUE,
		.depthCompareOp = VK_COMPARE_OP_LESS,
	};

	const VkPipelineColorBlendAttachmentState color_blend_attachment = {
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	const VkPipelineColorBlendStateCreateInfo color_blend = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color_blend_attachment,
	};

	const VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR,
	};
	const VkPipelineDynamicStateCreateInfo dynamic_state = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = sizeof(dynamic_states) / sizeof(dynamic_states[0]),
		.pDynamicStates = dynamic_states,
	};

	const VkPushConstantRange push_range = {
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(PushConstants),
	};
	const VkPipelineLayoutCreateInfo layout_info = {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push_range,
	};
	if (vkCreatePipelineLayout(ctx.device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
		std::cerr << "Failed to create pipeline layout\n";
		return false;
	}

	const VkGraphicsPipelineCreateInfo pipeline_info = {
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = sizeof(stages) / sizeof(stages[0]),
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport_state,
		.pRasterizationState = &rasterizer,
		.pMultisampleState = &multisample,
		.pDepthStencilState = &depth_stencil,
		.pColorBlendState = &color_blend,
		.pDynamicState = &dynamic_state,
		.layout = pipeline_layout,
		.renderPass = ctx.render_pass,
		.subpass = 0,
	};

	if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pipeline_info,
	                              nullptr, &pipeline) != VK_SUCCESS) {
		std::cerr << "Failed to create graphics pipeline\n";
		return false;
	}

	vkDestroyShaderModule(ctx.device, vert, nullptr);
	vkDestroyShaderModule(ctx.device, frag, nullptr);
	return true;
}

//положение куба
glm::vec3 currentPosition() {
	glm::vec3 p = object_position;
	if (traj_enabled) {
		constexpr float two_pi = 6.28318530718f;
		p.x += traj_radius * std::sin(two_pi * (traj_freq.x * anim_time + traj_phase.x));
		p.y += traj_radius * std::sin(two_pi * (traj_freq.y * anim_time + traj_phase.y));
		p.z += traj_radius * std::sin(two_pi * (traj_freq.z * anim_time + traj_phase.z));
	}
	return p;
}

glm::mat4 buildModelMatrix() {
	glm::mat4 m(1.0f);
	m = glm::translate(m, currentPosition());
	m = glm::rotate(m, glm::radians(object_rotation.y), {0.0f, 1.0f, 0.0f});
	m = glm::rotate(m, glm::radians(object_rotation.x), {1.0f, 0.0f, 0.0f});
	m = glm::rotate(m, glm::radians(object_rotation.z), {0.0f, 0.0f, 1.0f});
	m = glm::scale(m, object_scale);
	return m;
}

glm::mat4 buildViewMatrix() {
	glm::mat4 cam(1.0f);
	cam = glm::translate(cam, camera_pos);
	cam = glm::rotate(cam, glm::radians(camera_yaw), {0.0f, 1.0f, 0.0f});
	cam = glm::rotate(cam, glm::radians(camera_pitch), {1.0f, 0.0f, 0.0f});
	return glm::inverse(cam);
}

glm::mat4 buildProjectionMatrix(float aspect) {
	glm::mat4 p;
	if (projection_mode == 0) {
		p = glm::perspective(glm::radians(fov_deg), aspect, near_plane, far_plane);
	} else {
		const float h = ortho_size;
		p = glm::ortho(-h * aspect, h * aspect, -h, h, near_plane, far_plane);
	}
	p[1][1] *= -1.0f; //тк ось у вулкана направлена вниз
	return p;
}

}

bool initialize() {
	if (!createBufferWithData(sizeof(cube_vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	                          cube_vertices, vertex_buffer, vertex_buffer_allocation)) {
		std::cerr << "Failed to create cube vertex buffer\n";
		return false;
	}
	if (!createBufferWithData(sizeof(cube_indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
	                          cube_indices, index_buffer, index_buffer_allocation)) {
		std::cerr << "Failed to create cube index buffer\n";
		return false;
	}
	if (!createPipeline()) {
		return false;
	}
	return true;
}

void shutdown() {
	auto& ctx = graphics::internal::context;
	vkQueueWaitIdle(ctx.graphics_queue);

	vkDestroyPipeline(ctx.device, pipeline, nullptr);
	vkDestroyPipelineLayout(ctx.device, pipeline_layout, nullptr);
	vmaDestroyBuffer(ctx.allocator, index_buffer, index_buffer_allocation);
	vmaDestroyBuffer(ctx.allocator, vertex_buffer, vertex_buffer_allocation);
}

void update(double time) {
	static double last_time = time;
	const float dt = float(time - last_time);
	last_time = time;

	const float anim_dt = anim_playing ? dt * anim_speed : 0.0f;
	anim_time += anim_dt;

	if (auto_rotate) {
		object_rotation += rot_speed * anim_dt;
		for (int i = 0; i < 3; ++i) {
			if (object_rotation[i] > 360.0f)  object_rotation[i] -= 360.0f;
			if (object_rotation[i] < -360.0f) object_rotation[i] += 360.0f;
		}
	}

	ImGui::Begin("Scene");

	if (ImGui::CollapsingHeader("Projection", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::RadioButton("Perspective", &projection_mode, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Orthographic", &projection_mode, 1);

		if (projection_mode == 0) {
			ImGui::SliderFloat("FOV", &fov_deg, 20.0f, 120.0f, "%.1f deg");
		} else {
			ImGui::SliderFloat("Size", &ortho_size, 0.5f, 20.0f);
		}
		ImGui::SliderFloat("Near", &near_plane, 0.01f, 5.0f);
		ImGui::SliderFloat("Far", &far_plane, 5.0f, 200.0f);
	}

	if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::DragFloat3("Position", &object_position.x, 0.01f);
		ImGui::DragFloat3("Rotation (deg)", &object_rotation.x, 1.0f);
		ImGui::DragFloat3("Scale", &object_scale.x, 0.01f, 0.01f, 10.0f);
		if (ImGui::Button("Reset transform")) {
			object_position = {0.0f, 0.0f, 0.0f};
			object_rotation = {0.0f, 0.0f, 0.0f};
			object_scale = {1.0f, 1.0f, 1.0f};
		}
	}

	if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::ColorEdit3("Tint", &object_tint.x);
		ImGui::TextWrapped("Умножается на процедурный цвет вершин "
		                   "(RGB = локальная позиция, нормализованная в [0, 1]).");
		if (ImGui::Button("White")) {
			object_tint = {1.0f, 1.0f, 1.0f, 1.0f};
		}
	}

	if (ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Button(anim_playing ? "Pause" : "Play")) {
			anim_playing = !anim_playing;
		}
		ImGui::SameLine();
		if (ImGui::Button("Reset time")) {
			anim_time = 0.0f;
		}
		ImGui::SameLine();
		ImGui::Text("t = %.2f s", anim_time);

		ImGui::SliderFloat("Speed", &anim_speed, 0.0f, 3.0f, "%.2fx");

		ImGui::Separator();
		ImGui::Checkbox("Trajectory (Lissajous)", &traj_enabled);
		ImGui::SliderFloat("Radius", &traj_radius, 0.0f, 5.0f);
		ImGui::SliderFloat3("Freq (X, Y, Z)", &traj_freq.x, 0.0f, 3.0f, "%.2f Hz");
		ImGui::SliderFloat3("Phase", &traj_phase.x, 0.0f, 1.0f, "%.2f turn");

		ImGui::Separator();
		ImGui::Checkbox("Auto-rotate", &auto_rotate);
		ImGui::SliderFloat3("Rot speed (deg/s)", &rot_speed.x, -180.0f, 180.0f);
	}

	if (ImGui::CollapsingHeader("Camera")) {
		ImGui::DragFloat3("Position", &camera_pos.x, 0.05f);
		ImGui::SliderFloat("Yaw", &camera_yaw, -180.0f, 180.0f);
		ImGui::SliderFloat("Pitch", &camera_pitch, -89.0f, 89.0f);
		if (ImGui::Button("Reset camera")) {
			camera_pos = {0.0f, 0.0f, 5.0f};
			camera_yaw = 0.0f;
			camera_pitch = 0.0f;
		}
	}

	ImGui::End();
}

void render(const graphics::internal::FrameData& fd) {
	auto& ctx = graphics::internal::context;
	const VkCommandBuffer cmd = fd.command_buffer;

	vkResetCommandBuffer(cmd, 0);

	const VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vkBeginCommandBuffer(cmd, &begin_info);

	const VkClearValue clear_values[] = {
		{.color = {{0.05f, 0.06f, 0.09f, 1.0f}}},
		{.depthStencil = {1.0f, 0}},
	};

	const VkRenderPassBeginInfo render_pass_begin = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = ctx.render_pass,
		.framebuffer = fd.framebuffer,
		.renderArea = {.extent = ctx.swapchain_extent},
		.clearValueCount = sizeof(clear_values) / sizeof(clear_values[0]),
		.pClearValues = clear_values,
	};
	vkCmdBeginRenderPass(cmd, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	const VkViewport viewport = {
		.x = 0.0f,
		.y = 0.0f,
		.width = float(ctx.swapchain_extent.width),
		.height = float(ctx.swapchain_extent.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	const VkRect2D scissor = {.extent = ctx.swapchain_extent};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

	const VkDeviceSize vb_offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &vb_offset);
	vkCmdBindIndexBuffer(cmd, index_buffer, 0, VK_INDEX_TYPE_UINT16);

	const float aspect = float(ctx.swapchain_extent.width) /
	                     float(ctx.swapchain_extent.height);
	const PushConstants pc = {
		.mvp = buildProjectionMatrix(aspect) * buildViewMatrix() * buildModelMatrix(),
		.tint = object_tint,
	};
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
	                   sizeof(pc), &pc);

	vkCmdDrawIndexed(cmd, sizeof(cube_indices) / sizeof(cube_indices[0]),
	                 1, 0, 0, 0);

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

}
