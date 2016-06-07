/*
* Vulkan Example - Physically Based Rendering
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <gli/gli.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"

#define VERTEX_BUFFER_BIND_ID 0
#define ENABLE_VALIDATION false

// Vertex layout for this example
std::vector<vkMeshLoader::VertexLayout> vertexLayout =
{
	vkMeshLoader::VERTEX_LAYOUT_POSITION,
	vkMeshLoader::VERTEX_LAYOUT_NORMAL,
	vkMeshLoader::VERTEX_LAYOUT_UV
};

class VulkanExample : public VulkanExampleBase
{
public:
	bool displaySkybox = false;

	vkTools::VulkanTexture cubeMap;

	struct {
		VkPipelineVertexInputStateCreateInfo inputState;
		std::vector<VkVertexInputBindingDescription> bindingDescriptions;
		std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
	} vertices;

	struct {
		vkMeshLoader::MeshBuffer skybox;
		std::vector<vkMeshLoader::MeshBuffer> objects;
		uint32_t objectIndex = 3;
	} meshes;

	struct {
		vkTools::UniformData objectVS;
		vkTools::UniformData skyboxVS;
		vkTools::UniformData material;
	} uniformData;

	struct {
		glm::mat4 projection;
		glm::mat4 model;
		glm::vec4 lightPos = { 0.0f, 0.0f, 1.0f, 0.0f };
		float lodBias = 0.0f;
	} uboVS;

	struct Material {
		float roughness = 0.2f; // 0.3f;
		float F0 = 0.55f; // 0.8f;
		float k = 0.05f; // 0.2f 
		float _pad0;
		glm::vec4 color = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
//		glm::vec4 color = glm::vec4(0.9f, 0.1f, 0.1f, 1.0f);
	} uboMaterial;

	// From https://seblagarde.wordpress.com/2011/08/17/feeding-a-physical-based-lighting-mode/
	std::vector<glm::vec3> materialColors =
	{
		glm::vec3(0.9f, 0.1f, 0.1f), // Default red
		glm::vec3(0.971519f, 0.959915f, 0.915324f), // Silver
		glm::vec3(0.913183f, 0.921494f, 0.924524f), // Aluminium
		glm::vec3(1.0f, 0.765557f, 0.336057f) // Gold
	};
	uint32_t matColorIndex = 0;
	/*
		Gold        1           0.765557    0.336057
		Copper      0.955008    0.637427    0.538163
		Chromium    0.549585    0.556114    0.554256
		Nickel      0.659777    0.608679    0.525649
		Titanium    0.541931    0.496791    0.449419
		Cobalt      0.662124    0.654864    0.633732
		Platinum    0.672411    0.637331    0.585456
	*/

	struct {
		VkPipeline skybox;
		VkPipeline reflect;
	} pipelines;

	struct {
		VkDescriptorSet object;
		VkDescriptorSet skybox;
	} descriptorSets;

	struct {
		VkDescriptorSetLayout object;
		VkDescriptorSetLayout skybox;
	} descriptorSetLayouts;

	struct {
		VkPipelineLayout object;
		VkPipelineLayout skybox;
	} pipelineLayouts;


	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		zoom = -4.0f;
		rotationSpeed = 0.25f;
		rotation = glm::vec3(0.0f);
		enableTextOverlay = true;
		title = "Vulkan Example - PBR";
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources 
		// Note : Inherited destructor cleans up resources stored in base class

		// Clean up texture resources
		vkDestroyImageView(device, cubeMap.view, nullptr);
		vkDestroyImage(device, cubeMap.image, nullptr);
		vkDestroySampler(device, cubeMap.sampler, nullptr);
		vkFreeMemory(device, cubeMap.deviceMemory, nullptr);

		vkDestroyPipeline(device, pipelines.skybox, nullptr);
		vkDestroyPipeline(device, pipelines.reflect, nullptr);

		vkDestroyPipelineLayout(device, pipelineLayouts.object, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayouts.skybox, nullptr);

		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.object, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.skybox, nullptr);

		for (size_t i = 0; i < meshes.objects.size(); i++)
		{
			vkMeshLoader::freeMeshBufferResources(device, &meshes.objects[i]);
		}
		vkMeshLoader::freeMeshBufferResources(device, &meshes.skybox);

		vkTools::destroyUniformData(device, &uniformData.objectVS);
		vkTools::destroyUniformData(device, &uniformData.skyboxVS);
		vkTools::destroyUniformData(device, &uniformData.material);
	}

	void loadCubemap(std::string filename, VkFormat format, bool forceLinearTiling)
	{
#if defined(__ANDROID__)
		// Textures are stored inside the apk on Android (compressed)
		// So they need to be loaded via the asset manager
		AAsset* asset = AAssetManager_open(androidApp->activity->assetManager, filename.c_str(), AASSET_MODE_STREAMING);
		assert(asset);
		size_t size = AAsset_getLength(asset);
		assert(size > 0);

		void *textureData = malloc(size);
		AAsset_read(asset, textureData, size);
		AAsset_close(asset);

		gli::textureCube texCube(gli::load((const char*)textureData, size));
#else
		gli::textureCube texCube(gli::load(filename));
#endif

		assert(!texCube.empty());

		cubeMap.width = texCube.dimensions().x;
		cubeMap.height = texCube.dimensions().y;
		cubeMap.mipLevels = texCube.levels();

		VkMemoryAllocateInfo memAllocInfo = vkTools::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;

		// Create a host-visible staging buffer that contains the raw image data
		VkBuffer stagingBuffer;
		VkDeviceMemory stagingMemory;

		VkBufferCreateInfo bufferCreateInfo = vkTools::initializers::bufferCreateInfo();
		bufferCreateInfo.size = texCube.size();
		// This buffer is used as a transfer source for the buffer copy
		bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VK_CHECK_RESULT(vkCreateBuffer(device, &bufferCreateInfo, nullptr, &stagingBuffer));

		// Get memory requirements for the staging buffer (alignment, memory type bits)
		vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
		memAllocInfo.allocationSize = memReqs.size;
		// Get memory type index for a host visible buffer
		memAllocInfo.memoryTypeIndex = getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &stagingMemory));
		VK_CHECK_RESULT(vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0));

		// Copy texture data into staging buffer
		uint8_t *data;
		VK_CHECK_RESULT(vkMapMemory(device, stagingMemory, 0, memReqs.size, 0, (void **)&data));
		memcpy(data, texCube.data(), texCube.size());
		vkUnmapMemory(device, stagingMemory);

		// Create optimal tiled target image
		VkImageCreateInfo imageCreateInfo = vkTools::initializers::imageCreateInfo();
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.format = format;
		imageCreateInfo.mipLevels = cubeMap.mipLevels;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		imageCreateInfo.extent = { cubeMap.width, cubeMap.height, 1 };
		imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		// Cube faces count as array layers in Vulkan
		imageCreateInfo.arrayLayers = 6;
		// This flag is required for cube map images
		imageCreateInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

		VK_CHECK_RESULT(vkCreateImage(device, &imageCreateInfo, nullptr, &cubeMap.image));

		vkGetImageMemoryRequirements(device, cubeMap.image, &memReqs);

		memAllocInfo.allocationSize = memReqs.size;
		memAllocInfo.memoryTypeIndex = getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &cubeMap.deviceMemory));
		VK_CHECK_RESULT(vkBindImageMemory(device, cubeMap.image, cubeMap.deviceMemory, 0));

		VkCommandBuffer copyCmd = VulkanExampleBase::createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

		// Setup buffer copy regions for each face including all of it's miplevels
		std::vector<VkBufferImageCopy> bufferCopyRegions;
		uint32_t offset = 0;

		for (uint32_t face = 0; face < 6; face++)
		{
			for (uint32_t level = 0; level < cubeMap.mipLevels; level++)
			{
				VkBufferImageCopy bufferCopyRegion = {};
				bufferCopyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				bufferCopyRegion.imageSubresource.mipLevel = level;
				bufferCopyRegion.imageSubresource.baseArrayLayer = face;
				bufferCopyRegion.imageSubresource.layerCount = 1;
				bufferCopyRegion.imageExtent.width = texCube[face][level].dimensions().x;
				bufferCopyRegion.imageExtent.height = texCube[face][level].dimensions().y;
				bufferCopyRegion.imageExtent.depth = 1;
				bufferCopyRegion.bufferOffset = offset;

				bufferCopyRegions.push_back(bufferCopyRegion);

				// Increase offset into staging buffer for next level / face
				offset += texCube[face][level].size();
			}
		}

		// Image barrier for optimal image (target)
		// Set initial layout for all array layers (faces) of the optimal (target) tiled texture
		VkImageSubresourceRange subresourceRange = {};
		subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		subresourceRange.baseMipLevel = 0;
		subresourceRange.levelCount = cubeMap.mipLevels;
		subresourceRange.layerCount = 6;

		vkTools::setImageLayout(
			copyCmd,
			cubeMap.image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_PREINITIALIZED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			subresourceRange);

		// Copy the cube map faces from the staging buffer to the optimal tiled image
		vkCmdCopyBufferToImage(
			copyCmd,
			stagingBuffer,
			cubeMap.image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			static_cast<uint32_t>(bufferCopyRegions.size()),
			bufferCopyRegions.data()
			);

		// Change texture image layout to shader read after all faces have been copied
		cubeMap.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkTools::setImageLayout(
			copyCmd,
			cubeMap.image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			cubeMap.imageLayout,
			subresourceRange);

		VulkanExampleBase::flushCommandBuffer(copyCmd, queue, true);

		// Create sampler
		VkSamplerCreateInfo sampler = vkTools::initializers::samplerCreateInfo();
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = sampler.addressModeU;
		sampler.addressModeW = sampler.addressModeU;
		sampler.mipLodBias = 0.0f;
		sampler.maxAnisotropy = 8.0f;
		sampler.compareOp = VK_COMPARE_OP_NEVER;
		sampler.minLod = 0.0f;
		sampler.maxLod = cubeMap.mipLevels;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &cubeMap.sampler));

		// Create image view
		VkImageViewCreateInfo view = vkTools::initializers::imageViewCreateInfo();
		// Cube map view type
		view.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
		view.format = format;
		view.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
		view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		// 6 array layers (faces)
		view.subresourceRange.layerCount = 6;
		// Set number of mip levels
		view.subresourceRange.levelCount = cubeMap.mipLevels;
		view.image = cubeMap.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &view, nullptr, &cubeMap.view));

		// Clean up staging resources
		vkFreeMemory(device, stagingMemory, nullptr);
		vkDestroyBuffer(device, stagingBuffer, nullptr);
	}

	void reBuildCommandBuffers()
	{
		if (!checkCommandBuffers())
		{
			destroyCommandBuffers();
			createCommandBuffers();
		}
		buildCommandBuffers();
	}

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vkTools::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = defaultClearColor;
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vkTools::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			// Set target frame buffer
			renderPassBeginInfo.framebuffer = frameBuffers[i];

			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = vkTools::initializers::viewport((float)width,	(float)height, 0.0f, 1.0f);
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

			VkRect2D scissor = vkTools::initializers::rect2D(width,	height,	0, 0);
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

			VkDeviceSize offsets[1] = { 0 };

			// Skybox
			if (displaySkybox)
			{
				vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.skybox, 0, 1, &descriptorSets.skybox, 0, NULL);
				vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &meshes.skybox.vertices.buf, offsets);
				vkCmdBindIndexBuffer(drawCmdBuffers[i], meshes.skybox.indices.buf, 0, VK_INDEX_TYPE_UINT32);
				vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.skybox);
				vkCmdDrawIndexed(drawCmdBuffers[i], meshes.skybox.indexCount, 1, 0, 0, 0);
			}

			// 3D object
			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.object, 0, 1, &descriptorSets.object, 0, NULL);
			vkCmdBindVertexBuffers(drawCmdBuffers[i], VERTEX_BUFFER_BIND_ID, 1, &meshes.objects[meshes.objectIndex].vertices.buf, offsets);
			vkCmdBindIndexBuffer(drawCmdBuffers[i], meshes.objects[meshes.objectIndex].indices.buf, 0, VK_INDEX_TYPE_UINT32);
			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.reflect);
			vkCmdDrawIndexed(drawCmdBuffers[i], meshes.objects[meshes.objectIndex].indexCount, 1, 0, 0, 0);

			vkCmdEndRenderPass(drawCmdBuffers[i]);

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void loadMeshes()
	{
		// Skybox
		loadMesh(getAssetPath() + "models/cube.obj", &meshes.skybox, vertexLayout, 0.05f);
		// Objects
		meshes.objects.resize(5);
		loadMesh(getAssetPath() + "models/geosphere.obj", &meshes.objects[0], vertexLayout, 0.05f);
		loadMesh(getAssetPath() + "models/teapot.dae", &meshes.objects[1], vertexLayout, 0.05f);
		loadMesh(getAssetPath() + "models/torusknot.obj", &meshes.objects[2], vertexLayout, 0.05f);
		loadMesh(getAssetPath() + "models/suzanne.obj", &meshes.objects[3], vertexLayout, 0.15f);
		loadMesh(getAssetPath() + "models/chinesedragon.dae", &meshes.objects[4], vertexLayout, 0.3f);
	}

	void setupVertexDescriptions()
	{
		// Binding description
		vertices.bindingDescriptions.resize(1);
		vertices.bindingDescriptions[0] =
			vkTools::initializers::vertexInputBindingDescription(
				VERTEX_BUFFER_BIND_ID,
				vkMeshLoader::vertexSize(vertexLayout),
				VK_VERTEX_INPUT_RATE_VERTEX);

		// Attribute descriptions
		// Describes memory layout and shader positions
		vertices.attributeDescriptions.resize(3);
		// Location 0 : Position
		vertices.attributeDescriptions[0] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				0,
				VK_FORMAT_R32G32B32_SFLOAT,
				0);
		// Location 1 : Normal
		vertices.attributeDescriptions[1] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				1,
				VK_FORMAT_R32G32B32_SFLOAT,
				sizeof(float) * 3);
		// Location 2 : Texture coordinates
		vertices.attributeDescriptions[2] =
			vkTools::initializers::vertexInputAttributeDescription(
				VERTEX_BUFFER_BIND_ID,
				2,
				VK_FORMAT_R32G32_SFLOAT,
				sizeof(float) * 5);

		vertices.inputState = vkTools::initializers::pipelineVertexInputStateCreateInfo();
		vertices.inputState.vertexBindingDescriptionCount = vertices.bindingDescriptions.size();
		vertices.inputState.pVertexBindingDescriptions = vertices.bindingDescriptions.data();
		vertices.inputState.vertexAttributeDescriptionCount = vertices.attributeDescriptions.size();
		vertices.inputState.pVertexAttributeDescriptions = vertices.attributeDescriptions.data();
	}

	void setupDescriptorPool()
	{
		std::vector<VkDescriptorPoolSize> poolSizes =
		{
			vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3),
			vkTools::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2)
		};

		VkDescriptorPoolCreateInfo descriptorPoolInfo = 
			vkTools::initializers::descriptorPoolCreateInfo(
				poolSizes.size(),
				poolSizes.data(),
				2);

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
	}

	void setupDescriptorSetLayout()
	{
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings;
		VkDescriptorSetLayoutCreateInfo descriptorLayout;
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo;

		// Object
		// Binding 0 : Vertex shader uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1 : Fragment shader material uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));
		// Binding 1 : Fragment shader image sampler
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			2));

		descriptorLayout = vkTools::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), setLayoutBindings.size());
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.object));

		pipelineLayoutCreateInfo = vkTools::initializers::pipelineLayoutCreateInfo(&descriptorSetLayouts.object, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.object));

		// Skybox
		setLayoutBindings.clear();
		// Binding 0 : Vertex shader uniform buffer
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			VK_SHADER_STAGE_VERTEX_BIT,
			0));
		// Binding 1 : Fragment shader image sampler
		setLayoutBindings.push_back(vkTools::initializers::descriptorSetLayoutBinding(
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			VK_SHADER_STAGE_FRAGMENT_BIT,
			1));

		descriptorLayout = vkTools::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings.data(), setLayoutBindings.size());
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayouts.skybox));

		pipelineLayoutCreateInfo = vkTools::initializers::pipelineLayoutCreateInfo(&descriptorSetLayouts.skybox, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayouts.skybox));
	}

	void setupDescriptorSets()
	{
		// Image descriptor for the cube map texture
		VkDescriptorImageInfo cubeMapDescriptor =
			vkTools::initializers::descriptorImageInfo(
				cubeMap.sampler,
				cubeMap.view,
				VK_IMAGE_LAYOUT_GENERAL);

		VkDescriptorSetAllocateInfo setAllocInfo;
		std::vector<VkWriteDescriptorSet> writeDescriptorSets;

		// 3Object
		setAllocInfo = vkTools::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.object, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSets.object));
		// Binding 0 : Vertex shader uniform buffer
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSets.object,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			0,
			&uniformData.objectVS.descriptor));
		// Binding 0 : Fragment shader material uniform buffer
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSets.object,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			1,
			&uniformData.material.descriptor));
		// Binding 1 : Fragment shader cubemap sampler
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSets.object,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			2,
			&cubeMapDescriptor));
		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);

		// Sky box descriptor set
		setAllocInfo = vkTools::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.skybox, 1);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &setAllocInfo, &descriptorSets.skybox));
		writeDescriptorSets.clear();
		// Binding 0 : Vertex shader uniform buffer
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSets.skybox,
			VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
			0,
			&uniformData.skyboxVS.descriptor));
			// Binding 1 : Fragment shader cubemap sampler
		writeDescriptorSets.push_back(vkTools::initializers::writeDescriptorSet(
			descriptorSets.skybox,
			VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			1,
			&cubeMapDescriptor));
		vkUpdateDescriptorSets(device, writeDescriptorSets.size(), writeDescriptorSets.data(), 0, NULL);
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState =
			vkTools::initializers::pipelineInputAssemblyStateCreateInfo(
				VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
				0,
				VK_FALSE);

		VkPipelineRasterizationStateCreateInfo rasterizationState =
			vkTools::initializers::pipelineRasterizationStateCreateInfo(
				VK_POLYGON_MODE_FILL,
				VK_CULL_MODE_BACK_BIT,
				VK_FRONT_FACE_COUNTER_CLOCKWISE,
				0);

		VkPipelineColorBlendAttachmentState blendAttachmentState =
			vkTools::initializers::pipelineColorBlendAttachmentState(
				0xf,
				VK_FALSE);

		VkPipelineColorBlendStateCreateInfo colorBlendState =
			vkTools::initializers::pipelineColorBlendStateCreateInfo(
				1, 
				&blendAttachmentState);

		VkPipelineDepthStencilStateCreateInfo depthStencilState =
			vkTools::initializers::pipelineDepthStencilStateCreateInfo(
				VK_FALSE,
				VK_FALSE,
				VK_COMPARE_OP_LESS_OR_EQUAL);

		VkPipelineViewportStateCreateInfo viewportState =
			vkTools::initializers::pipelineViewportStateCreateInfo(1, 1, 0);

		VkPipelineMultisampleStateCreateInfo multisampleState =
			vkTools::initializers::pipelineMultisampleStateCreateInfo(
				VK_SAMPLE_COUNT_1_BIT,
				0);

		std::vector<VkDynamicState> dynamicStateEnables = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR
		};
		VkPipelineDynamicStateCreateInfo dynamicState =
			vkTools::initializers::pipelineDynamicStateCreateInfo(
				dynamicStateEnables.data(),
				dynamicStateEnables.size(),
				0);

		// Skybox pipeline (background cube)
		std::array<VkPipelineShaderStageCreateInfo,2> shaderStages;

		shaderStages[0] = loadShader(getAssetPath() + "shaders/pbr/skybox.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/pbr/skybox.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		VkGraphicsPipelineCreateInfo pipelineCreateInfo =
			vkTools::initializers::pipelineCreateInfo(
				pipelineLayouts.skybox,
				renderPass,
				0);

		pipelineCreateInfo.pVertexInputState = &vertices.inputState;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyState;
		pipelineCreateInfo.pRasterizationState = &rasterizationState;
		pipelineCreateInfo.pColorBlendState = &colorBlendState;
		pipelineCreateInfo.pMultisampleState = &multisampleState;
		pipelineCreateInfo.pViewportState = &viewportState;
		pipelineCreateInfo.pDepthStencilState = &depthStencilState;
		pipelineCreateInfo.pDynamicState = &dynamicState;
		pipelineCreateInfo.stageCount = shaderStages.size();
		pipelineCreateInfo.pStages = shaderStages.data();

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.skybox));

		// Object rendering pipeline
		shaderStages[0] = loadShader(getAssetPath() + "shaders/pbr/brdf.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getAssetPath() + "shaders/pbr/brdf.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		// Enable depth test and write
		depthStencilState.depthWriteEnable = VK_TRUE;
		depthStencilState.depthTestEnable = VK_TRUE;
		// Flip cull mode
		rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT;
		pipelineCreateInfo.layout = pipelineLayouts.object;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCreateInfo, nullptr, &pipelines.reflect));
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Object
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			sizeof(uboVS),
			nullptr,
			&uniformData.objectVS.buffer,
			&uniformData.objectVS.memory,
			&uniformData.objectVS.descriptor);

		// Material 
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			sizeof(uboMaterial),
			nullptr,
			&uniformData.material.buffer,
			&uniformData.material.memory,
			&uniformData.material.descriptor);

		// Skybox
		createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			sizeof(uboVS),
			nullptr,
			&uniformData.skyboxVS.buffer,
			&uniformData.skyboxVS.memory,
			&uniformData.skyboxVS.descriptor);

		updateUniformBuffers();
		updateMaterialBuffer();
	}

	void updateUniformBuffers()
	{
		// 3D object
		glm::mat4 viewMatrix = glm::mat4();
		uboVS.projection = glm::perspective(glm::radians(60.0f), (float)width / (float)height, 0.001f, 256.0f);
		viewMatrix = glm::translate(viewMatrix, glm::vec3(0.0f, 0.0f, zoom));

		uboVS.model = glm::mat4();
		uboVS.model = viewMatrix * glm::translate(uboVS.model, cameraPos);
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformData.objectVS.memory, 0, sizeof(uboVS), 0, (void **)&pData));
		memcpy(pData, &uboVS, sizeof(uboVS));
		vkUnmapMemory(device, uniformData.objectVS.memory);

		// Skybox
		viewMatrix = glm::mat4();
		uboVS.projection = glm::perspective(glm::radians(60.0f), (float)width / (float)height, 0.001f, 256.0f);

		uboVS.model = glm::mat4();
		uboVS.model = viewMatrix * glm::translate(uboVS.model, glm::vec3(0, 0, 0));
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
		uboVS.model = glm::rotate(uboVS.model, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

		VK_CHECK_RESULT(vkMapMemory(device, uniformData.skyboxVS.memory, 0, sizeof(uboVS), 0, (void **)&pData));
		memcpy(pData, &uboVS, sizeof(uboVS));
		vkUnmapMemory(device, uniformData.skyboxVS.memory);
	}

	void updateMaterialBuffer()
	{
		uboMaterial.color = glm::vec4(materialColors[matColorIndex], 1.0f);
		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformData.material.memory, 0, VK_WHOLE_SIZE, 0, (void **)&pData));
		memcpy(pData, &uboMaterial, sizeof(uboMaterial));
		vkUnmapMemory(device, uniformData.material.memory);
	}

	void updateMaterial()
	{
		if (enableTextOverlay && textOverlay->visible)
		{
			updateTextOverlay();
		}
		updateMaterialBuffer();
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();

		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		loadMeshes();
		setupVertexDescriptions();
		prepareUniformBuffers();
		loadCubemap(
			getAssetPath() + "textures/cubemap_yokohama.ktx",
			VK_FORMAT_BC3_UNORM_BLOCK,
			false);
		setupDescriptorSetLayout();
		preparePipelines();
		setupDescriptorPool();
		setupDescriptorSets();
		buildCommandBuffers();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
	}

	void toggleSkyBox()
	{
		displaySkybox = !displaySkybox;
		reBuildCommandBuffers();
	}

	void toggleObject()
	{
		meshes.objectIndex++;
		if (meshes.objectIndex >= static_cast<uint32_t>(meshes.objects.size()))
		{
			meshes.objectIndex = 0;
		}
		reBuildCommandBuffers();
	}

	void changeLodBias(float delta)
	{
		uboVS.lodBias += delta;
		if (uboVS.lodBias < 0.0f)
		{
			uboVS.lodBias = 0.0f;
		}
		if (uboVS.lodBias > cubeMap.mipLevels)
		{
			uboVS.lodBias = cubeMap.mipLevels;
		}
		updateUniformBuffers();
		updateTextOverlay();
	}

	virtual void keyPressed(uint32_t keyCode)
	{
		switch (keyCode)
		{
		case 0x53:
		case GAMEPAD_BUTTON_A:
			toggleSkyBox();
			break;
		case 0x20:
		case GAMEPAD_BUTTON_X:
			toggleObject();
			break;
		case 0x6B:
		case GAMEPAD_BUTTON_R1:
			changeLodBias(0.1f);
			break;
		case 0x6D:
		case GAMEPAD_BUTTON_L1:
			changeLodBias(-0.1f);
			break;
		// Material
#if defined(_WIN32)
		case 0x52:
		{
			float delta = (GetAsyncKeyState(VK_LSHIFT) < 0) ? 0.05f : -0.05f;
			uboMaterial.roughness = fmax(0.0f, fmin(uboMaterial.roughness + delta, 1.0f));
			updateMaterial();
			break;
		}
		case 0x46:
		{
			float delta = (GetAsyncKeyState(VK_LSHIFT) < 0) ? 0.05f : -0.05f;
			uboMaterial.F0 = fmax(0.0f, fmin(uboMaterial.F0 + delta, 1.0f));
			updateMaterial();
			break;
		}
		case 0x4B:
		{
			float delta = (GetAsyncKeyState(VK_LSHIFT) < 0) ? 0.05f : -0.05f;
			uboMaterial.k = fmax(0.0f, fmin(uboMaterial.k + delta, 1.0f));
			updateMaterial();
			break;
		}
		case 0x4D:
		{
			int32_t delta = (GetAsyncKeyState(VK_LSHIFT) < 0) ? 1 : -1;
			if ((delta > 0) || (matColorIndex > 0))
				matColorIndex += delta;
			if (matColorIndex >= materialColors.size()-1)
				matColorIndex = materialColors.size()-1;
			updateMaterial();
			break;
		}
#endif
		}
	}

	virtual void getOverlayText(VulkanTextOverlay *textOverlay)
	{
#if defined(__ANDROID__)
#else
		std::stringstream ss;
		textOverlay->addText("Material parameter:", 5.0f, 85.0f, VulkanTextOverlay::alignLeft);
		ss << "roughness: " << std::setprecision(2) << std::fixed << uboMaterial.roughness << " (r/R)";
		textOverlay->addText(ss.str(), 5.0f, 105.0f, VulkanTextOverlay::alignLeft);
		ss.str("");
		ss << "F0: " << std::setprecision(2) << std::fixed << uboMaterial.F0 << " (f/F)";
		textOverlay->addText(ss.str(), 5.0f, 125.0f, VulkanTextOverlay::alignLeft);
		ss.str("");
		ss << "k: " << std::setprecision(2) << std::fixed << uboMaterial.k << " (k/K)";
		textOverlay->addText(ss.str(), 5.0f, 145.0f, VulkanTextOverlay::alignLeft);
		ss.str("");
		ss << "color: " << std::setprecision(2) << std::fixed << uboMaterial.color.r << "/" << uboMaterial.color.g << "/" << uboMaterial.color.b << " (m/M)";
		textOverlay->addText(ss.str(), 5.0f, 165.0f, VulkanTextOverlay::alignLeft);
#endif
	}
};

VulkanExample *vulkanExample;

#if defined(_WIN32)
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleMessages(hWnd, uMsg, wParam, lParam);
	}
	return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}
#elif defined(__linux__) && !defined(__ANDROID__)
static void handleEvent(const xcb_generic_event_t *event)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleEvent(event);
	}
}
#endif

// Main entry point
#if defined(_WIN32)
// Windows entry point
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow)
#elif defined(__ANDROID__)
// Android entry point
void android_main(android_app* state)
#elif defined(__linux__)
// Linux entry point
int main(const int argc, const char *argv[])
#endif
{
#if defined(__ANDROID__)
	// Removing this may cause the compiler to omit the main entry point 
	// which would make the application crash at start
	app_dummy();
#endif
	vulkanExample = new VulkanExample();
#if defined(_WIN32)
	vulkanExample->setupWindow(hInstance, WndProc);
#elif defined(__ANDROID__)
	// Attach vulkan example to global android application state
	state->userData = vulkanExample;
	state->onAppCmd = VulkanExample::handleAppCommand;
	state->onInputEvent = VulkanExample::handleAppInput;
	vulkanExample->androidApp = state;
#elif defined(__linux__)
	vulkanExample->setupWindow();
#endif
#if !defined(__ANDROID__)
	vulkanExample->initSwapchain();
	vulkanExample->prepare();
#endif
	vulkanExample->renderLoop();
	delete(vulkanExample);
#if !defined(__ANDROID__)
	return 0;
#endif
}