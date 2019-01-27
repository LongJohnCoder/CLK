//
//  ScanTarget.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 05/11/2018.
//  Copyright © 2018 Thomas Harte. All rights reserved.
//

#include "ScanTarget.hpp"

#include "Primitives/Rectangle.hpp"

#include <cassert>
#include <cstring>

using namespace Outputs::Display::OpenGL;

namespace {

/// The texture unit from which to source input data.
constexpr GLenum SourceDataTextureUnit = GL_TEXTURE0;

/// The texture unit which contains raw line-by-line composite, S-Video or RGB data.
constexpr GLenum UnprocessedLineBufferTextureUnit = GL_TEXTURE1;

/// The texture unit that contains the current display.
constexpr GLenum AccumulationTextureUnit = GL_TEXTURE2;

#define TextureAddress(x, y)	(((y) << 11) | (x))
#define TextureAddressGetY(v)	uint16_t((v) >> 11)
#define TextureAddressGetX(v)	uint16_t((v) & 0x7ff)
#define TextureSub(a, b)		(((a) - (b)) & 0x3fffff)

const GLint internalFormatForDepth(std::size_t depth) {
	switch(depth) {
		default: return GL_FALSE;
		case 1: return GL_R8UI;
		case 2: return GL_RG8UI;
		case 3: return GL_RGB8UI;
		case 4: return GL_RGBA8UI;
	}
}

const GLenum formatForDepth(std::size_t depth) {
	switch(depth) {
		default: return GL_FALSE;
		case 1: return GL_RED_INTEGER;
		case 2: return GL_RG_INTEGER;
		case 3: return GL_RGB_INTEGER;
		case 4: return GL_RGBA_INTEGER;
	}
}

}

template <typename T> void ScanTarget::allocate_buffer(const T &array, GLuint &buffer_name, GLuint &vertex_array_name) {
	const auto buffer_size = array.size() * sizeof(array[0]);
	glGenBuffers(1, &buffer_name);
	glBindBuffer(GL_ARRAY_BUFFER, buffer_name);
	glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(buffer_size), NULL, GL_STREAM_DRAW);

	glGenVertexArrays(1, &vertex_array_name);
	glBindVertexArray(vertex_array_name);
	glBindBuffer(GL_ARRAY_BUFFER, buffer_name);
}

ScanTarget::ScanTarget(GLuint target_framebuffer, float output_gamma) :
	target_framebuffer_(target_framebuffer),
	output_gamma_(output_gamma),
	unprocessed_line_texture_(LineBufferWidth, LineBufferHeight, UnprocessedLineBufferTextureUnit, GL_NEAREST, false),
	full_display_rectangle_(-1.0f, -1.0f, 2.0f, 2.0f) {

	// Ensure proper initialisation of the two atomic pointer sets.
	read_pointers_.store(write_pointers_);
	submit_pointers_.store(write_pointers_);

	// Allocate space for the scans and lines.
	allocate_buffer(scan_buffer_, scan_buffer_name_, scan_vertex_array_);
	allocate_buffer(line_buffer_, line_buffer_name_, line_vertex_array_);

	// TODO: if this is OpenGL 4.4 or newer, use glBufferStorage rather than glBufferData
	// and specify GL_MAP_PERSISTENT_BIT. Then map the buffer now, and let the client
	// write straight into it.

	glGenTextures(1, &write_area_texture_name_);

	glBlendFunc(GL_SRC_ALPHA, GL_CONSTANT_COLOR);
	glBlendColor(0.4f, 0.4f, 0.4f, 1.0f);

	is_drawing_.clear();
}

ScanTarget::~ScanTarget() {
	while(is_drawing_.test_and_set());
	glDeleteBuffers(1, &scan_buffer_name_);
	glDeleteTextures(1, &write_area_texture_name_);
	glDeleteVertexArrays(1, &scan_vertex_array_);
}

void ScanTarget::set_target_framebuffer(GLuint target_framebuffer) {
	while(is_drawing_.test_and_set());
	target_framebuffer_ = target_framebuffer;
	is_drawing_.clear();
}

void ScanTarget::set_modals(Modals modals) {
	// Don't change the modals while drawing is ongoing; a previous set might be
	// in the process of being established.
	while(is_drawing_.test_and_set());
	modals.display_type = DisplayType::CompositeMonochrome;
	modals_ = modals;
	modals_are_dirty_ = true;
	is_drawing_.clear();
}

Outputs::Display::ScanTarget::Scan *ScanTarget::begin_scan() {
	if(allocation_has_failed_) return nullptr;

	const auto result = &scan_buffer_[write_pointers_.scan_buffer];
	const auto read_pointers = read_pointers_.load();

	// Advance the pointer.
	const auto next_write_pointer = decltype(write_pointers_.scan_buffer)((write_pointers_.scan_buffer + 1) % scan_buffer_.size());

	// Check whether that's too many.
	if(next_write_pointer == read_pointers.scan_buffer) {
		allocation_has_failed_ = true;
		return nullptr;
	}
	write_pointers_.scan_buffer = next_write_pointer;
	++provided_scans_;

	// Fill in extra OpenGL-specific details.
	result->line = write_pointers_.line;

	vended_scan_ = result;
	return &result->scan;
}

void ScanTarget::end_scan() {
	if(vended_scan_) {
		vended_scan_->data_y = TextureAddressGetY(vended_write_area_pointer_);
		vended_scan_->line = write_pointers_.line;
		vended_scan_->scan.end_points[0].data_offset += TextureAddressGetX(vended_write_area_pointer_);
		vended_scan_->scan.end_points[1].data_offset += TextureAddressGetX(vended_write_area_pointer_);
	}
	vended_scan_ = nullptr;
}

uint8_t *ScanTarget::begin_data(size_t required_length, size_t required_alignment) {
	if(allocation_has_failed_) return nullptr;
	if(!write_area_texture_.size()) {
		allocation_has_failed_ = true;
		return nullptr;
	}

	// Determine where the proposed write area would start and end.
	uint16_t output_y = TextureAddressGetY(write_pointers_.write_area);

	uint16_t aligned_start_x = TextureAddressGetX(write_pointers_.write_area & 0xffff) + 1;
	aligned_start_x += uint16_t((required_alignment - aligned_start_x%required_alignment)%required_alignment);

	uint16_t end_x = aligned_start_x + uint16_t(1 + required_length);

	if(end_x > WriteAreaWidth) {
		output_y = (output_y + 1) % WriteAreaHeight;
		aligned_start_x = uint16_t(required_alignment);
		end_x = aligned_start_x + uint16_t(1 + required_length);
	}

	// Check whether that steps over the read pointer.
	const auto end_address = TextureAddress(end_x, output_y);
	const auto read_pointers = read_pointers_.load();

	const auto end_distance = TextureSub(end_address, read_pointers.write_area);
	const auto previous_distance = TextureSub(write_pointers_.write_area, read_pointers.write_area);

	// If allocating this would somehow make the write pointer back away from the read pointer,
	// there must not be enough space left.
	if(end_distance < previous_distance) {
		allocation_has_failed_ = true;
		return nullptr;
	}

	// Everything checks out, return the pointer.
	vended_write_area_pointer_ = write_pointers_.write_area = TextureAddress(aligned_start_x, output_y);
	return &write_area_texture_[size_t(write_pointers_.write_area) * data_type_size_];

	// Note state at exit:
	//		write_pointers_.write_area points to the first pixel the client is expected to draw to.
}

void ScanTarget::end_data(size_t actual_length) {
	if(allocation_has_failed_) return;

	// Bookend the start of the new data, to safeguard for precision errors in sampling.
	memcpy(
		&write_area_texture_[size_t(write_pointers_.write_area - 1) * data_type_size_],
		&write_area_texture_[size_t(write_pointers_.write_area) * data_type_size_],
		data_type_size_);

	// The write area was allocated in the knowledge that there's sufficient
	// distance left on the current line, so there's no need to worry about carry.
	write_pointers_.write_area += actual_length + 1;

	// Also bookend the end.
	memcpy(
		&write_area_texture_[size_t(write_pointers_.write_area - 1) * data_type_size_],
		&write_area_texture_[size_t(write_pointers_.write_area - 2) * data_type_size_],
		data_type_size_);
}

void ScanTarget::submit() {
	if(allocation_has_failed_) {
		// Reset all pointers to where they were; this also means
		// the stencil won't be properly populated.
		write_pointers_ = submit_pointers_.load();
		frame_is_complete_ = false;
	} else {
		// Advance submit pointer.
		submit_pointers_.store(write_pointers_);
	}

	allocation_has_failed_ = false;
}

void ScanTarget::announce(Event event, bool is_visible, const Outputs::Display::ScanTarget::Scan::EndPoint &location, uint8_t composite_amplitude) {
	if(event == ScanTarget::Event::EndVerticalRetrace) {
		// The previous-frame-is-complete flag is subject to a two-slot queue because
		// measurement for *this* frame needs to begin now, meaning that the previous
		// result needs to be put somewhere. Setting frame_is_complete_ back to true
		// only after it has been put somewhere also doesn't work, since if the first
		// few lines of a frame are skipped for any reason, there'll be nowhere to
		// put it.
		is_first_in_frame_ = true;
		previous_frame_was_complete_ = frame_is_complete_;
		frame_is_complete_ = true;
	}

	if(output_is_visible_ == is_visible) return;
	if(is_visible) {
		// Commit the most recent line only if any scans fell on it.
		// Otherwise there's no point outputting it, it'll contribute nothing.
		if(provided_scans_) {
			// Store metadata if concluding a previous line.
			if(active_line_) {
				line_metadata_buffer_[size_t(write_pointers_.line)].is_first_in_frame = is_first_in_frame_;
				line_metadata_buffer_[size_t(write_pointers_.line)].previous_frame_was_complete = previous_frame_was_complete_;
				is_first_in_frame_ = false;
			}

			const auto read_pointers = read_pointers_.load();

			// Attempt to allocate a new line; note allocation failure if necessary.
			const auto next_line = uint16_t((write_pointers_.line + 1) % LineBufferHeight);
			if(next_line == read_pointers.line) {
				allocation_has_failed_ = true;
				active_line_ = nullptr;
			} else {
				write_pointers_.line = next_line;
				active_line_ = &line_buffer_[size_t(write_pointers_.line)];
			}
			provided_scans_ = 0;
		}

		if(active_line_) {
			active_line_->end_points[0].x = location.x;
			active_line_->end_points[0].y = location.y;
			active_line_->end_points[0].cycles_since_end_of_horizontal_retrace = location.cycles_since_end_of_horizontal_retrace;
			active_line_->end_points[0].composite_angle = location.composite_angle;
			active_line_->line = write_pointers_.line;
			active_line_->composite_amplitude = composite_amplitude;
		}
	} else {
		if(active_line_) {
			active_line_->end_points[1].x = location.x;
			active_line_->end_points[1].y = location.y;
			active_line_->end_points[1].cycles_since_end_of_horizontal_retrace = location.cycles_since_end_of_horizontal_retrace;
			active_line_->end_points[1].composite_angle = location.composite_angle;
		}
	}
	output_is_visible_ = is_visible;
}

void ScanTarget::setup_pipeline() {
	const auto data_type_size = Outputs::Display::size_for_data_type(modals_.input_data_type);
	if(data_type_size != data_type_size_) {
		// TODO: flush output.

		data_type_size_ = data_type_size;
		write_area_texture_.resize(2048*2048*data_type_size_);

		write_pointers_.scan_buffer = 0;
		write_pointers_.write_area = 0;
	}

	// Pick a processing width; this will be the minimum necessary not to
	// lose any detail when combining the input.
	processing_width_ = modals_.cycles_per_line / modals_.clocks_per_pixel_greatest_common_divisor;

	// Establish an output shader.
	output_shader_ = conversion_shader();
	glBindVertexArray(line_vertex_array_);
	glBindBuffer(GL_ARRAY_BUFFER, line_buffer_name_);
	enable_vertex_attributes(ShaderType::Conversion, *output_shader_);
	set_uniforms(ShaderType::Conversion, *output_shader_);
	output_shader_->set_uniform("origin", modals_.visible_area.origin.x, modals_.visible_area.origin.y);
	output_shader_->set_uniform("size", modals_.visible_area.size.width, modals_.visible_area.size.height);
	output_shader_->set_uniform("textureName", GLint(UnprocessedLineBufferTextureUnit - GL_TEXTURE0));

	// Establish an input shader.
	input_shader_ = composition_shader();
	glBindVertexArray(scan_vertex_array_);
	glBindBuffer(GL_ARRAY_BUFFER, scan_buffer_name_);
	enable_vertex_attributes(ShaderType::Composition, *input_shader_);
	set_uniforms(ShaderType::Composition, *input_shader_);
	input_shader_->set_uniform("textureName", GLint(SourceDataTextureUnit - GL_TEXTURE0));
}

void ScanTarget::draw(bool synchronous, int output_width, int output_height) {
	if(fence_ != nullptr) {
		// if the GPU is still busy, don't wait; we'll catch it next time
		if(glClientWaitSync(fence_, GL_SYNC_FLUSH_COMMANDS_BIT, synchronous ? GL_TIMEOUT_IGNORED : 0) == GL_TIMEOUT_EXPIRED) {
			return;
		}
		fence_ = nullptr;
	}

	// Spin until the is-drawing flag is reset; the wait sync above will deal
	// with instances where waiting is inappropriate.
	while(is_drawing_.test_and_set());

	// Establish the pipeline if necessary.
	if(modals_are_dirty_) {
		setup_pipeline();
		modals_are_dirty_ = false;
	}

	// Grab the current read and submit pointers.
	const auto submit_pointers = submit_pointers_.load();
	const auto read_pointers = read_pointers_.load();

	// Submit scans; only the new ones need to be communicated.
	size_t new_scans = (submit_pointers.scan_buffer + scan_buffer_.size() - read_pointers.scan_buffer) % scan_buffer_.size();
	if(new_scans) {
		glBindBuffer(GL_ARRAY_BUFFER, scan_buffer_name_);

		// Map only the required portion of the buffer.
		const size_t new_scans_size = new_scans * sizeof(Scan);
		uint8_t *const destination = static_cast<uint8_t *>(
			glMapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(new_scans_size), GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT)
		);

		if(read_pointers.scan_buffer < submit_pointers.scan_buffer) {
			memcpy(destination, &scan_buffer_[read_pointers.scan_buffer], new_scans_size);
		} else {
			const size_t first_portion_length = (scan_buffer_.size() - read_pointers.scan_buffer) * sizeof(Scan);
			memcpy(destination, &scan_buffer_[read_pointers.scan_buffer], first_portion_length);
			memcpy(&destination[first_portion_length], &scan_buffer_[0], new_scans_size - first_portion_length);
		}

		// Flush and unmap the buffer.
		glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(new_scans_size));
		glUnmapBuffer(GL_ARRAY_BUFFER);
	}

	// Submit texture.
	if(submit_pointers.write_area != read_pointers.write_area) {
		glActiveTexture(SourceDataTextureUnit);
		glBindTexture(GL_TEXTURE_2D, write_area_texture_name_);

		// Create storage for the texture if it doesn't yet exist; this was deferred until here
		// because the pixel format wasn't initially known.
		if(!texture_exists_) {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexImage2D(
				GL_TEXTURE_2D,
				0,
				internalFormatForDepth(data_type_size_),
				WriteAreaWidth,
				WriteAreaHeight,
				0,
				formatForDepth(data_type_size_),
				GL_UNSIGNED_BYTE,
				nullptr);
			texture_exists_ = true;
		}

		const auto start_y = TextureAddressGetY(read_pointers.write_area);
		const auto end_y = TextureAddressGetY(submit_pointers.write_area);
		if(end_y >= start_y) {
			// Submit the direct region from the submit pointer to the read pointer.
			glTexSubImage2D(	GL_TEXTURE_2D, 0,
								0, start_y,
								WriteAreaWidth,
								1 + end_y - start_y,
								formatForDepth(data_type_size_),
								GL_UNSIGNED_BYTE,
								&write_area_texture_[size_t(TextureAddress(0, start_y)) * data_type_size_]);
		} else {
			// The circular buffer wrapped around; submit the data from the read pointer to the end of
			// the buffer and from the start of the buffer to the submit pointer.
			glTexSubImage2D(	GL_TEXTURE_2D, 0,
								0, 0,
								WriteAreaWidth,
								1 + end_y,
								formatForDepth(data_type_size_),
								GL_UNSIGNED_BYTE,
								&write_area_texture_[0]);
			glTexSubImage2D(	GL_TEXTURE_2D, 0,
								0, start_y,
								WriteAreaWidth,
								WriteAreaHeight - start_y,
								formatForDepth(data_type_size_),
								GL_UNSIGNED_BYTE,
								&write_area_texture_[size_t(TextureAddress(0, start_y)) * data_type_size_]);
		}
	}

	// Push new input to the unprocessed line buffer.
	if(new_scans) {
		unprocessed_line_texture_.bind_framebuffer();

		// Clear newly-touched lines; that is everything from (read+1) to submit.
		const uint16_t first_line_to_clear = (read_pointers.line+1)%line_buffer_.size();
		const uint16_t final_line_to_clear = submit_pointers.line;
		if(first_line_to_clear != final_line_to_clear) {
			glEnable(GL_SCISSOR_TEST);

			if(first_line_to_clear < final_line_to_clear) {
				glScissor(0, first_line_to_clear, unprocessed_line_texture_.get_width(), final_line_to_clear - first_line_to_clear);
				glClear(GL_COLOR_BUFFER_BIT);
			} else {
				glScissor(0, 0, unprocessed_line_texture_.get_width(), final_line_to_clear);
				glClear(GL_COLOR_BUFFER_BIT);
				glScissor(0, first_line_to_clear, unprocessed_line_texture_.get_width(), unprocessed_line_texture_.get_height() - first_line_to_clear);
				glClear(GL_COLOR_BUFFER_BIT);
			}

			glDisable(GL_SCISSOR_TEST);
		}

		// Apply new spans. They definitely always go to the first buffer.
		glBindVertexArray(scan_vertex_array_);
		input_shader_->bind();
		glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(new_scans));
	}

	// Ensure the accumulation buffer is properly sized.
	const int proportional_width = (output_height * 4) / 3;
	if(!accumulation_texture_ || (	/* !synchronous && */ (accumulation_texture_->get_width() != proportional_width || accumulation_texture_->get_height() != output_height))) {
		std::unique_ptr<OpenGL::TextureTarget> new_framebuffer(
			new TextureTarget(
				GLsizei(proportional_width),
				GLsizei(output_height),
				AccumulationTextureUnit,
				GL_NEAREST,
				true));
		if(accumulation_texture_) {
			new_framebuffer->bind_framebuffer();
			glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

			glActiveTexture(AccumulationTextureUnit);
			accumulation_texture_->bind_texture();
			accumulation_texture_->draw(float(output_width) / float(output_height));

			glClear(GL_STENCIL_BUFFER_BIT);

			new_framebuffer->bind_texture();
		}
		accumulation_texture_ = std::move(new_framebuffer);

		// In the absence of a way to resize a stencil buffer, just mark
		// what's currently present as invalid to avoid an improper clear
		// for this frame.
		stencil_is_valid_ = false;
	}

	// Figure out how many new spans are ostensible ready; use two less than that.
	uint16_t new_spans = (submit_pointers.line + LineBufferHeight - read_pointers.line) % LineBufferHeight;
	if(new_spans) {
		// Bind the accumulation framebuffer.
		accumulation_texture_->bind_framebuffer();

		// Enable blending and stenciling, and ensure spans increment the stencil buffer.
		glEnable(GL_BLEND);
		glEnable(GL_STENCIL_TEST);
		glStencilFunc(GL_EQUAL, 0, GLuint(~0));
		glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);

		// Prepare to output lines.
		glBindVertexArray(line_vertex_array_);
		output_shader_->bind();

		// Prepare to upload data that will consitute lines.
		glBindBuffer(GL_ARRAY_BUFFER, line_buffer_name_);

		// Divide spans by which frame they're in.
		uint16_t start_line = read_pointers.line;
		while(new_spans) {
			uint16_t end_line = start_line+1;

			// Find the limit of spans to draw in this cycle.
			size_t spans = 1;
			while(end_line != submit_pointers.line && !line_metadata_buffer_[end_line].is_first_in_frame) {
				end_line = (end_line + 1) % LineBufferHeight;
				++spans;
			}

			// If this is start-of-frame, clear any untouched pixels and flush the stencil buffer
			if(line_metadata_buffer_[start_line].is_first_in_frame) {
				if(stencil_is_valid_ && line_metadata_buffer_[start_line].previous_frame_was_complete) {
					full_display_rectangle_.draw(0.0f, 0.0f, 0.0f);
				}
				stencil_is_valid_ = true;
				glClear(GL_STENCIL_BUFFER_BIT);

				// Rebind the program for span output.
				glBindVertexArray(line_vertex_array_);
				output_shader_->bind();
			}

			// Upload and draw.
			const auto buffer_size = spans * sizeof(Line);
			if(!end_line || end_line > start_line) {
				glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(buffer_size), &line_buffer_[start_line]);
			} else {
				uint8_t *destination = static_cast<uint8_t *>(
					glMapBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(buffer_size), GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT)
				);
				assert(destination);

				const size_t buffer_length = line_buffer_.size() * sizeof(Line);
				const size_t start_position = start_line * sizeof(Line);
				memcpy(&destination[0], &line_buffer_[start_line], buffer_length - start_position);
				memcpy(&destination[buffer_length - start_position], &line_buffer_[0], end_line * sizeof(Line));

				glFlushMappedBufferRange(GL_ARRAY_BUFFER, 0, GLsizeiptr(buffer_size));
				glUnmapBuffer(GL_ARRAY_BUFFER);
			}

			glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, GLsizei(spans));

			start_line = end_line;
			new_spans -= spans;
		}

		// Disable blending and the stencil test again.
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
	}

	// Copy the accumulatiion texture to the target.
	glBindFramebuffer(GL_FRAMEBUFFER, target_framebuffer_);
	glViewport(0, 0, (GLsizei)output_width, (GLsizei)output_height);

	glClear(GL_COLOR_BUFFER_BIT);
	accumulation_texture_->bind_texture();
	accumulation_texture_->draw(float(output_width) / float(output_height), 4.0f / 255.0f);

	// All data now having been spooled to the GPU, update the read pointers to
	// the submit pointer location.
	read_pointers_.store(submit_pointers);

	// Grab a fence sync object to avoid busy waiting upon the next extry into this
	// function, and reset the is_drawing_ flag.
	fence_ = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	is_drawing_.clear();
}
