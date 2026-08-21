#pragma once

#include "../Primitives/Matrix4x4.h"
#include "Material.h"
#include "Geometry.h"
#include "Texture.h"

namespace nCine
{
	class GLUniformCache;
	class GLUniformBlockCache;

	/**
		@brief Holds all the state needed to issue a single draw call
		
		Bundles the material, geometry, model transformation and sort keys for one renderable. The render
		queue sorts commands by their material sort key to minimize state changes, optionally merges them
		via @ref RenderBatcher, and finally calls @ref Issue() to bind the state and draw.
	*/
	class RenderCommand
	{
	public:
		/**
		 * @brief Command type
		 *
		 * Its sole purpose is to allow separated profiling counters in the `RenderStatistics` class.
		 */
		enum class Type
		{
			Unspecified = 0,	/**< Unspecified or non-profiled command */
			Sprite,				/**< Sprite draw command */
			MeshSprite,			/**< Mesh sprite draw command */
			TileMap,			/**< Tile map draw command */
			Particle,			/**< Particle draw command */
			Lighting,			/**< Lighting draw command */
			Text,				/**< Text draw command */
			ImGui,				/**< ImGui draw command */

			Count				/**< Number of command types */
		};

		explicit RenderCommand(Type type);
		RenderCommand();

		/** @brief Returns the number of instances collected in the command or zero if instancing is not used */
		inline std::int32_t GetInstanceCount() const {
			return numInstances_;
		}
		/** @brief Sets the number of instances collected in the command */
		inline void SetInstanceCount(std::int32_t numInstances) {
			numInstances_ = numInstances;
		}

		/** @brief Returns the number of elements collected by the command or zero if it's not a batch */
		inline std::int32_t GetBatchSize() const {
			return batchSize_;
		}
		/** @brief Sets the number of batch elements collected by the command */
		inline void SetBatchSize(std::int32_t batchSize) {
			batchSize_ = batchSize;
		}

		/** @brief Returns the drawing layer for this command */
		inline std::uint16_t GetLayer() const {
			return layer_;
		}
		/** @brief Sets the drawing layer for this command */
		inline void SetLayer(std::uint16_t layer) {
			layer_ = layer;
		}
		/** @brief Returns the visit order index for this command */
		inline std::uint16_t GetVisitOrder() const {
			return visitOrder_;
		}
		/** @brief Sets the visit order index for this command */
		inline void SetVisitOrder(std::uint16_t visitOrder) {
			visitOrder_ = visitOrder;
		}

		/** @brief Returns the material sort key for the queue */
		inline std::uint64_t GetMaterialSortKey() const {
			return materialSortKey_;
		}
		/** @brief Returns the lower part of the material sort key, used for batch splitting logic */
		inline std::uint32_t GetLowerMaterialSortKey() const {
			return std::uint32_t(materialSortKey_);
		}
		/** @brief Calculates the material sort key for the queue */
		void CalculateMaterialSortKey();
		/** @brief Returns the id based secondary sort key for the queue */
		inline std::uint32_t GetIdSortKey() const {
			return idSortKey_;
		}
		/** @brief Sets the id based secondary sort key for the queue */
		inline void SetIdSortKey(std::uint32_t idSortKey) {
			idSortKey_ = idSortKey;
		}

		/** @brief Binds the command state and issues the draw call */
		void Issue();

		/** @brief Returns the command type (for profiling purposes) */
		inline Type GetType() const {
#if defined(NCINE_PROFILING)
			return type_;
#else
			return Type::Unspecified;
#endif
		}
		/** @brief Sets the command type (for profiling purposes) */
		inline void SetType(Type type) {
#if defined(NCINE_PROFILING)
			type_ = type;
#endif
		}

		/** @brief Sets the scissor rectangle for this command */
		inline void SetScissor(Recti scissorRect) {
			scissorRect_ = scissorRect;
		}
		/** @brief Sets the scissor rectangle for this command */
		void SetScissor(GLint x, GLint y, GLsizei width, GLsizei height);

		/** @brief Returns the model transformation matrix */
		inline const Matrix4x4f& GetTransformation() const {
			return modelMatrix_;
		}
		/** @brief Sets the model transformation matrix and marks it for re-committing */
		void SetTransformation(const Matrix4x4f& modelMatrix);

		// PSP has no GL uniform reflection, so BaseSprite stashes the sprite parameters here (instead of the
		// InstanceBlock UBO) for the GU emitter to read at Issue time.
		float pspColor_[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float pspTexRect_[4] = { 1.0f, 0.0f, 1.0f, 0.0f };
		float pspSpriteSize_[2] = { 0.0f, 0.0f };
		// Flat offset into the 256x256 palette (_palettes + pspPaletteOffset_) for indexed (R8/RG8) sprites - the
		// GU emitter uses it to pick the palette row when baking/CLUT-loading. 0 = base palette.
		float pspPaletteOffset_ = 0.0f;
		bool pspHasSprite_ = false;
		// Screen-space (UI) command: its transformation is already in screen pixels (0..480 x 0..272), so the GU
		// emitter maps it straight to the framebuffer instead of through the (panning) scene camera. Set by the
		// UI Canvas/Font on PSP so the HUD and menus stay fixed on screen.
		bool pspUiSpace_ = false;
		// PSP bitmap-font rainbow path: the texture contains packed 4-bit coverage + 4-bit shader luminance.
		// PspEmitter expands it through a per-glyph CLUT to reproduce the desktop Colorized shader highlights.
		bool pspFontColorized_ = false;
		// Random-color text can share one neutral font CLUT and apply its dye per vertex, allowing all glyphs with
		// the same atlas to remain in the normal sprite batch. Non-rainbow shine keeps the colorized CLUT path.
		bool pspFontRainbow_ = false;
		// A cached PSP tile mesh can already be grouped by its baked 512x512 atlas page. `-1` keeps the generic
		// emitter-side classifier; non-negative values let the emitter bind that page directly.
		std::int16_t pspTexturePage_ = -1;
		// UI commands add this to their layer so they sort above the whole scene (PSP has no depth test, so paint
		// order = layer order). Well above any tilemap/actor layer yet within uint16; relative UI order is kept.
		static constexpr std::uint16_t PspUiLayerBase = 40000;
		inline void SetPspSpriteData(const float* color, float scaleX, float biasX, float scaleY, float biasY, float w, float h, float paletteOffset = 0.0f) {
			for (int i = 0; i < 4; ++i) pspColor_[i] = color[i];
			pspTexRect_[0] = scaleX; pspTexRect_[1] = biasX; pspTexRect_[2] = scaleY; pspTexRect_[3] = biasY;
			pspSpriteSize_[0] = w; pspSpriteSize_[1] = h; pspPaletteOffset_ = paletteOffset; pspHasSprite_ = true;
			pspFontColorized_ = false;
			pspFontRainbow_ = false;
			pspTexturePage_ = -1;
		}
		inline void ResetPspSpriteData() {
			pspHasSprite_ = false;
			pspUiSpace_ = false;
			pspFontColorized_ = false;
			pspFontRainbow_ = false;
			pspSpriteSize_[0] = 0.0f;
			pspSpriteSize_[1] = 0.0f;
			pspPaletteOffset_ = 0.0f;
			pspTexturePage_ = -1;
		}

		/** @brief Returns the material (read-only) */
		inline const Material& GetMaterial() const {
			return material_;
		}
		/** @brief Returns the geometry (read-only) */
		inline const Geometry& GetGeometry() const {
			return geometry_;
		}
		/** @brief Returns the material */
		inline Material& GetMaterial() {
			return material_;
		}
		/** @brief Returns the geometry */
		inline Geometry& GetGeometry() {
			return geometry_;
		}

		/** @brief Returns the material's "InstanceBlock" uniform block cache, resolved once per shader change */
		GLUniformBlockCache* GetInstanceBlock();

		/** @brief Commits the model matrix uniform block */
		void CommitNodeTransformation();

		/** @brief Commits the projection and view matrix uniforms */
		void CommitCameraTransformation();

		/** @brief Calls all the commit methods except the camera uniforms commit */
		void CommitAll();

		/** @brief Calculates the Z-depth of a command layer using the specified near and far planes */
		static float CalculateDepth(std::uint16_t layer, float nearClip, float farClip);

	private:
		/** @brief Distance on the Z axis between adjacent layers */
		static constexpr float LayerStep = 1.0f / static_cast<float>(0xFFFF);

		/** @brief Material sort key that minimizes state changes when rendering commands */
		std::uint64_t materialSortKey_;
		// Cached model matrix uniform and "InstanceBlock" uniform block cache, avoiding name-based
		// lookups on every use. Valid as long as the cached shader change counter matches the material's one.
		GLUniformCache* modelMatrixUniform_;
		GLUniformBlockCache* instanceBlock_;
		/** @brief Id based secondary sort key that stabilizes render command sorting */
		std::uint32_t idSortKey_;
		// Value of the material's shader change counter when the cached uniforms were resolved
		std::uint32_t cachedShaderChangeCounter_;
		/** @brief Drawing layer for this command */
		std::uint16_t layer_;
		/** @brief Visit order index for this command */
		std::uint16_t visitOrder_;
		std::int32_t numInstances_;
		std::int32_t batchSize_;

		bool transformationCommitted_;
		// Whether the cached model matrix uniform belongs to a uniform block (as opposed to a loose uniform)
		bool modelMatrixUniformInBlock_;

#if defined(NCINE_PROFILING)
		Type type_;
#endif

		Recti scissorRect_;
		Matrix4x4f modelMatrix_;
		Material material_;
		Geometry geometry_;

		/** @brief Returns the final layer sort key for this command */
		inline std::uint32_t GetLayerSortKey() const {
			return std::uint32_t(layer_ << 16) + visitOrder_;
		}

		// Re-resolves the cached uniform pointers if the material's shader program has changed
		void RefreshCachedUniforms();
	};
}
