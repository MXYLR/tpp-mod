#include <std_include.hpp>

#include "loader/component_loader.hpp"

#include "game/game.hpp"

#include "utils.hpp"
#include "fonts.hpp"
#include "draw.hpp"
#include "../console.hpp"
#include "../scheduler.hpp"
#include "../vars.hpp"

#include <utils/hook.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cwctype>

#pragma comment(lib, "gdi32.lib")

namespace renderer
{
	font_data_t font_data{};

	namespace
	{
		vars::var_ptr r_extra_font_1;
		vars::var_ptr r_extra_font_2;
		vars::var_ptr r_font_file;
		vars::var_ptr r_font_name;
		vars::var_ptr r_font_size;
		vars::var_ptr r_font_scale;
		vars::var_ptr r_font_x_offset;
		vars::var_ptr r_font_y_offset;
		vars::var_ptr r_font_ranges;
		vars::var_ptr r_font_extra;
		vars::var_ptr r_font_chinese;
		vars::var_ptr r_font_kanji_scale;
		vars::var_ptr r_font_latin_scale;

		// which built-in font a file replaces, so the runtime ttf font can be
		// sized to look like the font the game (or the translation) ships with
		enum class font_template_role
		{
			none,
			kanji,
			latin,
		};

		// a glyph box in 60ths of the text height, the unit the font metrics use
		struct ttf_glyph_target_t
		{
			bool valid = false;
			int width = 0;
			int height = 0;
			int horizontal_shift = 0;
			int vertical_shift = 0;
			int advance = 0;
		};

		// the reference character keeps its metrics rather than a box built from
		// them, because the box has to be built with the spacing the replacement
		// glyphs are placed with and that is not known until they are loaded
		struct font_template_t
		{
			bool valid = false;
			unsigned int character = 0;
			int reference_ink = 0;
			int reference_origin_x = 0;
			int reference_origin_y = 0;
			game::fox::gr::dg::FontTextureMetrics metrics{};
		};

		font_template_t kanji_font_template;
		font_template_t latin_font_template;

		constexpr float kanji_font_scale = 1.2f;
		constexpr char kanji_font_spacing = -6;
		constexpr float latin_font_scale = 1.f;
		constexpr char latin_font_spacing = -3;

		// the renderer reads the metrics as final values, while the game shifts the
		// glyph record on the way in. read back off a probe record: the width,
		// height and advance the game ends up with are the record's plus twice the
		// font spacing, the left bearing is the record's as it stands, and the top
		// is 60 - (record - 2 * spacing) because that one counts down from the top
		// of the line instead of up from the middle. a record meant to end up at a
		// given metric value therefore stores the inverse of that shift
		ttf_glyph_target_t target_from_metrics(const game::fox::gr::dg::FontTextureMetrics& metrics, const int spacing)
		{
			ttf_glyph_target_t target{};
			target.valid = metrics.f5 > 0.f && metrics.f6 > 0.f && metrics.f9 > 0.f;
			target.width = static_cast<int>(std::lround(metrics.f5 * 60.f)) - 2 * spacing;
			target.height = static_cast<int>(std::lround(metrics.f6 * 60.f)) - 2 * spacing;
			target.horizontal_shift = static_cast<int>(std::lround(metrics.f7 * 60.f)) - spacing;
			target.vertical_shift = static_cast<int>(std::lround((1.f - metrics.f8) * 60.f)) + 2 * spacing;
			target.advance = static_cast<int>(std::lround(metrics.f9 * 60.f)) - 2 * spacing;
			return target;
		}

		// the box the reference character has to end up with when the replacement
		// glyphs are placed with this spacing, which is what makes a glyph that
		// takes the reference cell over the same size as the characters around it
		ttf_glyph_target_t template_target(const font_template_t& reference, const int spacing)
		{
			return reference.valid ? target_from_metrics(reference.metrics, spacing) : ttf_glyph_target_t{};
		}

		// the reference character is looked up in the shared table instead of in the
		// font being loaded: the full width kanji live in font_def_jp, which is
		// loaded after KanjiFont, so searching the font at hand found none of the
		// samples and settled for its largest glyph instead - an arbitrary character
		// whose cell is both wider and taller than a normal kanji, and every kana
		// took that cell over
		void capture_font_template(const font_template_role role)
		{
			if (role == font_template_role::none)
			{
				return;
			}

			auto& result = role == font_template_role::kanji ? kanji_font_template : latin_font_template;
			if (result.valid)
			{
				return;
			}

			const auto kanji = role == font_template_role::kanji;
			const wchar_t samples[][4] =
			{
				{static_cast<wchar_t>(0x4E2D), static_cast<wchar_t>(0x56FD), static_cast<wchar_t>(0x65E5), static_cast<wchar_t>(0x76EE)},
				{L'H', L'E', L'B', L'0'}
			};

			auto picked = static_cast<unsigned int>(samples[kanji ? 0 : 1][0]);
			auto found = false;

			for (const auto character : samples[kanji ? 0 : 1])
			{
				if (font_data.wide_char_glyphs[static_cast<unsigned int>(character)].texture_handle == 0)
				{
					continue;
				}

				picked = static_cast<unsigned int>(character);
				found = true;
				break;
			}

			// a font that carries none of the samples still needs a box to measure
			// against, so the first character of the matching range stands in
			if (!found)
			{
				for (auto character = kanji ? 0x4E00u : 0x21u; character <= (kanji ? 0x9FFFu : 0x7Eu); character++)
				{
					if (font_data.wide_char_glyphs[character].texture_handle == 0)
					{
						continue;
					}

					picked = character;
					found = true;
					break;
				}
			}

			if (!found)
			{
				return;
			}

			result.metrics = font_data.wide_char_glyphs[picked].metrics;
			result.valid = true;
			result.character = picked;

			console::info("[font] %s reference glyph: 0x%X is %.2f x %.2f, left %.2f, top %.2f, advance %.2f\n",
				kanji ? "kanji" : "latin", result.character, result.metrics.f5 * 60.f, result.metrics.f6 * 60.f,
				result.metrics.f7 * 60.f, result.metrics.f8 * 60.f, result.metrics.f9 * 60.f);
		}

		void init_memory_manager(game::fox::gr::dg::MemoryManager* manager, const size_t size)
		{
			const auto aligned_size = (size + 15) & 0xFFFFFFFull;
			const auto buffer = game::fox::KernelAllocAligned(aligned_size, 0x10);

			manager->unk1 = reinterpret_cast<game::fox::gr::dg::MemoryManager_unk1*>(buffer);
			manager->size = aligned_size;

			manager->unk1->unk1 = 0;
			manager->unk1->unk2 = manager->unk1;
			manager->unk1->unk3 = aligned_size;
			manager->unk1->unk4 = 129;
		}

		void destroy_memory_manager(game::fox::gr::dg::MemoryManager* manager)
		{
			game::fox::FreeAnnotated(manager->unk1, 0x5000F);
			std::memset(manager, 0, sizeof(game::fox::gr::dg::MemoryManager));
		}

		void initialize_font_texture_render(game::fox::gr::dg::FontTextureRender* render)
		{
			utils::hook::set<size_t>(SELECT_VALUE(0x1402034DE, 0x140AD359E, 0x0, 0x0) + 2, 0x3DA16AC4E5A3); // force new texture resource
			game::fox::gr::dg::FontTextureRender_::FontTextureRender_(render, 0x800u, 0x400u);
			utils::hook::set<size_t>(SELECT_VALUE(0x1402034DE, 0x140AD359E, 0x0, 0x0) + 2, 0xB8A0BF169F98);
		}

		void destroy_font_texture_render(game::fox::gr::dg::FontTextureRender* render)
		{
			render->imageGlyphManager->__vftable->__destructor(render->imageGlyphManager, 1);
			render->imageAreaManager->__vftable->__destructor(render->imageAreaManager, 1);

			if (render->unk1)
			{
				game::fox::FreeAnnotated(render->unk1, 0);
			}

			if (render->unk2)
			{
				game::fox::FreeAnnotated(render->unk2, 0);
			}

			std::memset(render, 0, sizeof(game::fox::gr::dg::FontTextureRender));
		}

		int load_font_internal(game::fox::gr::dg::FontData* font, int begin, int max_count,
			float scale, char spacing,
			char x_offset, char y_offset)
		{
			game::fox::gr::dg::FontTextureRender render{};
			initialize_font_texture_render(&render);
			const auto _0 = gsl::finally([&]
			{
				destroy_font_texture_render(&render);
			});

			const auto end = std::min(begin + max_count, static_cast<int>(font->glyphCount));

			auto done_count = 0;
			for (auto i = begin; i < end; i++)
			{
				const auto glyph = &font->glyphs[i];
				const auto glyph_info = &font_data.wide_char_glyphs[glyph->character];

				unsigned char area_1 = glyph->width + 2 * (font->spacing + 4);
				unsigned char area_2 = font->unk1_3 + 2 * (font->spacing + 4);
				const auto area_info = game::fox::gr::dg::ImageAreaManager_::AllocateAreaInfo(
					render.imageAreaManager, area_1, area_2, glyph->character);

				if (area_info == nullptr)
				{
					// flush what was registered so far, otherwise those glyphs stay
					// permanently blank (their texture handle never gets pixels)
					game::fox::gr::dg::FontTextureRender_::UpdateRenderGlyphTexture(&render);
					console::info("[font] atlas full after %i glyphs (from index %i)\n", done_count, begin);
					return done_count;
				}

				const auto texture_glyph_data = game::fox::gr::dg::ImageGlyphManager_::RegisterGlyphData(
					render.imageGlyphManager, font, area_info, glyph->character);

				glyph_info->texture_handle = render.fontTextureHandle;
				game::fox::gr::dg::FontTextureRender_::AddTextureRenderOrder(&render, font, glyph, texture_glyph_data);

				texture_glyph_data->width = static_cast<unsigned char>(static_cast<float>(texture_glyph_data->width) * scale);
				texture_glyph_data->height = static_cast<unsigned char>(static_cast<float>(texture_glyph_data->height) * scale);
				texture_glyph_data->horizontalSpace += spacing;
				texture_glyph_data->horizontalShift += x_offset;
				texture_glyph_data->verticalShift += y_offset;

				const auto half_pixel_width = 0.5f / static_cast<float>(render.pixel_width);
				const auto half_pixel_height = 0.5f / static_cast<float>(render.pixel_height);
				game::fox::gr::dg::FontSystem_::CalculateMetrics(&glyph_info->metrics, texture_glyph_data, 
					half_pixel_width, half_pixel_height, 1.f / 60.f);

				++done_count;
			}
			
			game::fox::gr::dg::FontTextureRender_::UpdateRenderGlyphTexture(&render);
			return done_count;
		}

		void load_font(const char* font_name,
			float scale, char spacing,
			char x_offset, char y_offset)
		{
			game::fox::gr::dg::FontData font{};
			game::fox::gr::dg::MemoryManager memory_manager{};

			font.fontIndex = 0;
			font.memoryManager = &memory_manager;
			init_memory_manager(&memory_manager, 0x27D000);

			const auto _0 = gsl::finally([&]
			{
				destroy_memory_manager(&memory_manager);
			});

			if (!game::fox::gr::dg::FontData_::LoadFontData(&font, font_name, nullptr))
			{
				console::info("[font] failed to load \"%s\"\n", font_name);
				return;
			}

			// glyphCount is a 16 bit field, a file with more than 32767 glyphs
			// reports a negative count and cannot be loaded at all
			const auto glyph_count = static_cast<int>(font.glyphCount);
			if (glyph_count <= 0)
			{
				console::info("[font] \"%s\" has an invalid glyph count (%i), max is 32767 per file\n",
					font_name, glyph_count);
				return;
			}

			auto left_count = glyph_count;
			auto begin = 0;
			while (left_count > 0)
			{
				const auto max_count = std::min(256, left_count);
				const auto count = load_font_internal(&font, begin, max_count, scale, spacing, x_offset, y_offset);
				if (count <= 0)
				{
					console::info("[font] \"%s\" stopped at glyph %i/%i\n", font_name, begin, glyph_count);
					return;
				}

				left_count -= count;
				begin += count;
			}

			console::info("[font] loaded \"%s\" (%i glyphs)\n", font_name, glyph_count);
		}

		// ---------------------------------------------------------------------
		// runtime ttf/otf fonts
		//
		// the game renders fonts from a 1 bit per pixel source bitmap, so a ttf
		// file can be fed through the existing atlas/atlas-blit pipeline by
		// rasterizing it with gdi and building a FontData that points at that
		// bitmap. the game then handles atlas allocation, antialiasing, texture
		// creation and metrics exactly like it does for its own font files.
		// ---------------------------------------------------------------------

		// matches the size of the atlas the game allocates, so a source batch fills
		// as many atlases as it can
		constexpr int ttf_source_width = 2048;
		constexpr int ttf_source_height = 1024;
		constexpr int ttf_reference_size = 100;
		constexpr int ttf_font_spacing = 1;
		constexpr int ttf_max_batch = 1024;
		constexpr int ttf_max_characters = 32000;

		std::string wide_to_utf8(const std::wstring& input)
		{
			if (input.empty())
			{
				return {};
			}

			const auto size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
				nullptr, 0, nullptr, nullptr);
			if (size <= 0)
			{
				return {};
			}

			std::string result(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
				result.data(), size, nullptr, nullptr);
			return result;
		}

		std::wstring utf8_to_wide(const std::string& input)
		{
			if (input.empty())
			{
				return {};
			}

			const auto size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
			if (size <= 0)
			{
				return {};
			}

			std::wstring result(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), result.data(), size);
			return result;
		}

		std::wstring to_lower(std::wstring value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](const wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
			return value;
		}

		// strips everything but letters and digits, so "SourceHanSansCN-Regular.otf"
		// can be compared against the family name "Source Han Sans CN"
		std::wstring normalize_face_name(const std::wstring& value)
		{
			std::wstring result;
			for (const auto c : value)
			{
				if (iswalnum(c))
				{
					result.push_back(static_cast<wchar_t>(::towlower(c)));
				}
			}

			return result;
		}

		unsigned int read_be16(const unsigned char* data)
		{
			return (static_cast<unsigned int>(data[0]) << 8) | data[1];
		}

		unsigned int read_be32(const unsigned char* data)
		{
			return (static_cast<unsigned int>(data[0]) << 24) | (static_cast<unsigned int>(data[1]) << 16) |
				(static_cast<unsigned int>(data[2]) << 8) | data[3];
		}

		// the family a file registers under is the one from its own name table, and
		// that one is localized: SourceHanSansCN-Regular.otf registers as
		// "思源黑体 CN Regular" on a chinese system, while its file name is derived
		// from the english "Source Han Sans CN Regular". the file name alone
		// therefore cannot tell which of the installed Source Han Sans variants is
		// meant, and picking the wrong one silently renders the japanese or korean
		// glyph shapes instead of the chinese ones
		std::vector<std::wstring> read_font_names(const std::filesystem::path& file)
		{
			std::ifstream stream(file, std::ios::binary);
			if (!stream)
			{
				return {};
			}

			const auto read_at = [&stream](const unsigned long long offset, unsigned char* buffer, const size_t size)
			{
				stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
				stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(size));
				return static_cast<size_t>(stream.gcount()) == size;
			};

			unsigned char header[12]{};
			if (!read_at(0, header, sizeof header))
			{
				return {};
			}

			auto directory = 12ull;
			if (read_be32(header) == 0x74746366)  // 'ttcf', read the first face
			{
				unsigned char first_face[4]{};
				if (!read_at(12, first_face, sizeof first_face))
				{
					return {};
				}

				directory = read_be32(first_face);
				if (!read_at(directory, header, sizeof header))
				{
					return {};
				}
			}

			const auto table_count = read_be16(header + 4);
			if (table_count == 0 || table_count > 512)
			{
				return {};
			}

			std::vector<unsigned char> directory_data(static_cast<size_t>(table_count) * 16);
			if (!read_at(directory, directory_data.data(), directory_data.size()))
			{
				return {};
			}

			auto name_offset = 0ull;
			auto name_length = 0ull;
			for (auto i = 0u; i < table_count; i++)
			{
				const auto* entry = directory_data.data() + static_cast<size_t>(i) * 16;
				if (std::memcmp(entry, "name", 4) == 0)
				{
					name_offset = read_be32(entry + 8);
					name_length = read_be32(entry + 12);
					break;
				}
			}

			if (name_offset == 0 || name_length < 6 || name_length > 0x100000)
			{
				return {};
			}

			std::vector<unsigned char> table(static_cast<size_t>(name_length));
			if (!read_at(name_offset, table.data(), table.size()))
			{
				return {};
			}

			const auto count = read_be16(table.data() + 2);
			const auto string_offset = read_be16(table.data() + 4);
			if (count == 0 || count > 1024 || 6 + static_cast<size_t>(count) * 12 > table.size())
			{
				return {};
			}

			// the family name is what gdi matches against, the other names are kept to
			// confirm which file a family resolved to
			std::vector<std::pair<int, std::wstring>> found;

			for (auto i = 0u; i < count; i++)
			{
				const auto* record = table.data() + 6 + static_cast<size_t>(i) * 12;
				const auto platform = read_be16(record);
				const auto name_id = read_be16(record + 6);
				const auto length = read_be16(record + 8);
				const auto text_offset = read_be16(record + 10);

				auto priority = 0;
				switch (name_id)
				{
				case 1: priority = 0; break;   // family
				case 16: priority = 1; break;  // typographic family
				case 4: priority = 2; break;   // full name
				case 6: priority = 3; break;   // postscript name
				default: continue;
				}

				const auto begin = static_cast<size_t>(string_offset) + text_offset;
				if (length == 0 || begin + length > table.size())
				{
					continue;
				}

				std::wstring text;
				if (platform == 0 || platform == 3)
				{
					for (auto c = 0u; c + 1 < length; c += 2)
					{
						text.push_back(static_cast<wchar_t>(read_be16(table.data() + begin + c)));
					}
				}
				else if (platform == 1)
				{
					for (auto c = 0u; c < length; c++)
					{
						text.push_back(static_cast<wchar_t>(table[begin + c]));
					}
				}
				else
				{
					continue;
				}

				while (!text.empty() && (text.back() == L' ' || text.back() == L'\0'))
				{
					text.pop_back();
				}

				if (!text.empty())
				{
					found.emplace_back(priority, std::move(text));
				}
			}

			std::stable_sort(found.begin(), found.end(),
				[](const auto& left, const auto& right) { return left.first < right.first; });

			std::vector<std::wstring> names;
			for (auto& entry : found)
			{
				if (std::find(names.begin(), names.end(), entry.second) == names.end())
				{
					names.push_back(std::move(entry.second));
				}
			}

			return names;
		}

		HFONT create_gdi_font(const std::wstring& family, const int size)
		{
			return CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family.c_str());
		}

		// tells whether creating a font from this family really ends up on the given
		// file - gdi silently substitutes another font when the name does not match
		bool family_resolves_to_file(const HDC dc, const std::wstring& family, const std::filesystem::path& file,
			const std::vector<std::wstring>& declared)
		{
			const auto font = create_gdi_font(family, ttf_reference_size);
			if (font == nullptr)
			{
				return false;
			}

			const auto old_font = SelectObject(dc, font);
			auto result = false;

			// the total size of the font data identifies the file exactly, but a
			// collection reports the size of the face, not of the file
			const auto extension = to_lower(file.extension().wstring());
			if (extension != L".ttc" && extension != L".otc")
			{
				std::error_code error;
				const auto file_size = std::filesystem::file_size(file, error);
				result = !error && file_size != 0 &&
					static_cast<std::uintmax_t>(GetFontData(dc, 0, 0, nullptr, 0)) == file_size;
			}

			if (!result && !declared.empty())
			{
				const auto length = GetOutlineTextMetricsW(dc, 0, nullptr);
				if (length != 0)
				{
					std::vector<unsigned char> buffer(length);
					if (GetOutlineTextMetricsW(dc, length, reinterpret_cast<OUTLINETEXTMETRICW*>(buffer.data())))
					{
						const auto* metrics = reinterpret_cast<const OUTLINETEXTMETRICW*>(buffer.data());
						// the offsets are relative to the start of the structure
						const auto resolve = [&](const UINT_PTR offset)
						{
							return offset != 0 && offset < buffer.size()
								? normalize_face_name(reinterpret_cast<const wchar_t*>(buffer.data() + offset))
								: std::wstring{};
						};

						const auto full_name = resolve(reinterpret_cast<UINT_PTR>(metrics->otmpFullName));
						const auto family_name = resolve(reinterpret_cast<UINT_PTR>(metrics->otmpFamilyName));

						for (const auto& name : declared)
						{
							const auto normalized = normalize_face_name(name);
							if (normalized.size() < 4)
							{
								continue;
							}

							if (full_name.find(normalized) != std::wstring::npos ||
								family_name.find(normalized) != std::wstring::npos)
							{
								result = true;
								break;
							}
						}
					}
				}
			}

			SelectObject(dc, old_font);
			DeleteObject(font);
			return result;
		}

		int CALLBACK enum_font_families_callback(const LOGFONTW* font, const TEXTMETRICW*, DWORD, LPARAM param)
		{
			auto* families = reinterpret_cast<std::vector<std::wstring>*>(param);
			if (font->lfFaceName[0] != L'@')
			{
				families->emplace_back(font->lfFaceName);
			}

			return 1;
		}

		std::vector<std::wstring> enum_font_families(const HDC dc)
		{
			std::vector<std::wstring> families;

			LOGFONTW logfont{};
			logfont.lfCharSet = DEFAULT_CHARSET;
			EnumFontFamiliesExW(dc, &logfont, enum_font_families_callback,
				reinterpret_cast<LPARAM>(&families), 0);

			std::sort(families.begin(), families.end());
			families.erase(std::unique(families.begin(), families.end()), families.end());
			return families;
		}

		std::vector<std::filesystem::path> find_font_files(const std::wstring& path, const std::wstring& name)
		{
			std::error_code error;
			const std::filesystem::path root(path);

			if (!std::filesystem::is_directory(root, error))
			{
				return {root};
			}

			std::vector<std::filesystem::path> files;
			for (const auto& entry : std::filesystem::directory_iterator(root, error))
			{
				if (!entry.is_regular_file(error))
				{
					continue;
				}

				const auto extension = to_lower(entry.path().extension().wstring());
				if (extension == L".ttf" || extension == L".otf" || extension == L".ttc" || extension == L".otc")
				{
					files.push_back(entry.path());
				}
			}

			std::sort(files.begin(), files.end());

			// prefer a file whose name contains r_font_name
			if (!name.empty())
			{
				const auto lower_name = to_lower(name);
				for (auto it = files.begin(); it != files.end(); ++it)
				{
					if (to_lower(it->filename().wstring()).find(lower_name) != std::wstring::npos)
					{
						std::iter_swap(files.begin(), it);
						break;
					}
				}
			}

			return files;
		}

		// adds the file as a private font resource and returns the family name it
		// registered, picked by diffing the family list before and after
		bool add_private_font_file(const std::filesystem::path& file, const HDC dc, const std::wstring& name, std::wstring& family)
		{
			const auto before = enum_font_families(dc);

			if (AddFontResourceExW(file.wstring().c_str(), FR_PRIVATE, nullptr) == 0)
			{
				console::info("[font] \"%s\" is not a usable font file\n", wide_to_utf8(file.filename().wstring()).data());
				return false;
			}

			const auto after = enum_font_families(dc);

			std::vector<std::wstring> added;
			std::set_difference(after.begin(), after.end(), before.begin(), before.end(), std::back_inserter(added));

			// a font that is already installed does not show up in the diff, fall
			// back to the full list in that case
			const auto& candidates = added.empty() ? after : added;
			if (candidates.empty())
			{
				console::info("[font] \"%s\" did not register a font family\n", wide_to_utf8(file.filename().wstring()).data());
				return false;
			}

			const auto hint = normalize_face_name(name);
			const auto stem = normalize_face_name(file.stem().wstring());
			const auto declared = read_font_names(file);

			// the file names are also matched against, so the log shows the family
			// name the rest of the code knows the file by instead of the localized one
			const auto log_family = [&](const std::wstring& used, const bool confirmed, const bool guessed)
			{
				std::string identity;
				if (confirmed)
				{
					std::error_code error;
					const auto size = std::filesystem::file_size(file, error);
					identity = " (confirmed";
					if (!error)
					{
						identity += ", " + std::to_string(size) + " bytes";
					}
					identity += ")";
				}

				console::info("[font] \"%s\" uses family \"%s\"%s%s\n",
					wide_to_utf8(file.filename().wstring()).data(), wide_to_utf8(used).data(),
					identity.data(), guessed ? " (could not confirm it, set r_font_name to change it)" : "");
			};

			std::wstring best;
			size_t best_score = 0;

			for (const auto& candidate : candidates)
			{
				const auto normalized = normalize_face_name(candidate);
				if (normalized.empty())
				{
					continue;
				}

				auto score = size_t{};
				if (!hint.empty() && normalized.find(hint) != std::wstring::npos)
				{
					score = normalized.size() + 1;
				}
				else if (!hint.empty() && hint.find(normalized) != std::wstring::npos)
				{
					score = normalized.size() + 1;
				}
				else if (!stem.empty() && stem.find(normalized) != std::wstring::npos)
				{
					score = normalized.size();
				}

				if (score > best_score)
				{
					best_score = score;
					best = candidate;
				}
			}

			if (!best.empty() && family_resolves_to_file(dc, best, file, declared))
			{
				family = best;
				log_family(family, true, false);
				return true;
			}

			// the scored name did not end up on this file, so try the names the file
			// itself declares and keep the first one that does
			for (const auto& declared_name : declared)
			{
				if (!family_resolves_to_file(dc, declared_name, file, declared))
				{
					continue;
				}

				family = declared_name;
				log_family(family, true, false);
				return true;
			}

			family = best.empty() ? candidates.front() : best;
			log_family(family, false, true);
			return true;
		}

		struct ttf_glyph_bitmap_t
		{
			int width = 0;
			int height = 0;
			int origin_x = 0;
			int origin_y = 0;
			int advance = 0;
			std::vector<unsigned char> pixels;
		};

		bool rasterize_glyph(const HDC dc, const wchar_t character, std::vector<unsigned char>& scratch, ttf_glyph_bitmap_t& result)
		{
			GLYPHMETRICS metrics{};
			MAT2 transform{};
			transform.eM11.value = 1;
			transform.eM22.value = 1;

			const auto size = GetGlyphOutlineW(dc, static_cast<UINT>(character), GGO_GRAY8_BITMAP,
				&metrics, 0, nullptr, &transform);
			if (size == GDI_ERROR)
			{
				return false;
			}

			result.width = static_cast<int>(metrics.gmBlackBoxX);
			result.height = static_cast<int>(metrics.gmBlackBoxY);
			result.origin_x = metrics.gmptGlyphOrigin.x;
			result.origin_y = metrics.gmptGlyphOrigin.y;
			result.advance = static_cast<int>(metrics.gmCellIncX);
			result.pixels.assign(static_cast<size_t>(std::max(1, result.width)) * static_cast<size_t>(std::max(1, result.height)), 0);

			if (size <= 0 || result.width == 0 || result.height == 0)
			{
				return true;
			}

			scratch.resize(static_cast<size_t>(size));
			if (GetGlyphOutlineW(dc, static_cast<UINT>(character), GGO_GRAY8_BITMAP, &metrics,
				size, scratch.data(), &transform) == GDI_ERROR)
			{
				return false;
			}

			// the bitmap is coverage with rows padded to 4 bytes. how far up the ramp
			// goes is up to the driver: the documentation describes 0..64, but the one
			// this runs on tops out at 16, and a cutoff fixed at the top of the wider
			// ramp turned every glyph into an empty bitmap. the cutoff is therefore
			// taken against the ramp this bitmap actually uses, which leaves a driver
			// that really does hand back 0..64 with the same halfway cutoff as before
			const auto pitch = (result.width + 3) & ~3;
			auto peak = 0;
			for (auto y = 0; y < result.height; y++)
			{
				for (auto x = 0; x < result.width; x++)
				{
					peak = std::max(peak, static_cast<int>(scratch[static_cast<size_t>(y) * pitch + x]));
				}
			}

			for (auto y = 0; y < result.height; y++)
			{
				for (auto x = 0; x < result.width; x++)
				{
					if (static_cast<int>(scratch[static_cast<size_t>(y) * pitch + x]) * 2 > peak)
					{
						result.pixels[static_cast<size_t>(y) * result.width + x] = 1;
					}
				}
			}

			return true;
		}

		// returns the ink of the tallest of the sample characters, used to scale a ttf
		// so its glyphs end up the size the fonts the game ships with draw them at
		ttf_glyph_bitmap_t measure_reference_glyph(const HDC dc, const wchar_t* samples, const size_t count)
		{
			std::vector<unsigned char> scratch;
			ttf_glyph_bitmap_t best{};

			for (auto i = 0u; i < count; i++)
			{
				ttf_glyph_bitmap_t bitmap{};
				if (rasterize_glyph(dc, samples[i], scratch, bitmap) && bitmap.height > best.height)
				{
					best = std::move(bitmap);
				}
			}

			return best;
		}

		std::vector<wchar_t> parse_character_ranges(const std::string& ranges)
		{
			std::vector<wchar_t> characters;

			auto text = ranges;
			size_t position = 0;

			while (position < text.size())
			{
				const auto separator = text.find(',', position);
				auto entry = text.substr(position, separator == std::string::npos ? std::string::npos : separator - position);
				position = separator == std::string::npos ? text.size() : separator + 1;

				entry.erase(std::remove_if(entry.begin(), entry.end(), [](const char c) { return std::isspace(static_cast<unsigned char>(c)); }), entry.end());
				if (entry.empty())
				{
					continue;
				}

				const auto dash = entry.find('-');
				const auto first = static_cast<unsigned int>(std::strtoul(entry.data(), nullptr, 0));
				const auto last = dash == std::string::npos
					? first
					: static_cast<unsigned int>(std::strtoul(entry.data() + dash + 1, nullptr, 0));

				for (auto c = first; c <= last && c <= 0xFFFF; c++)
				{
					characters.push_back(static_cast<wchar_t>(c));
				}
			}

			return characters;
		}

		// the game's own fonts are japanese, so their kanji stop well short of what
		// chinese text uses. a character the game has no glyph for cannot be drawn
		// at all, and which ones a translation needs is not knowable up front, so
		// the common chinese set is requested whole. gb2312 level 1 is the 3755
		// characters behind about 99.7% of running text, level 2 the rarer rest
		std::vector<wchar_t> common_chinese_characters(const int level)
		{
			std::vector<wchar_t> characters;
			if (level <= 0)
			{
				return characters;
			}

			const auto last_high = level >= 2 ? 0xF7 : 0xD7;
			for (auto high = 0xB0; high <= last_high; high++)
			{
				for (auto low = 0xA1; low <= 0xFE; low++)
				{
					const char bytes[2] = { static_cast<char>(high), static_cast<char>(low) };
					wchar_t character = 0;
					if (MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, bytes, 2, &character, 1) != 1)
					{
						// the row has holes in it, those byte pairs are not a character
						continue;
					}

					characters.push_back(character);
				}
			}

			return characters;
		}

		std::vector<wchar_t> collect_ttf_characters(const std::string& ranges, const std::string& extra, const int chinese)
		{
			std::vector<wchar_t> characters;
			auto native_count = 0;

			// everything the built-in fonts already provide
			for (auto c = 1; c < 0xFFFF; c++)
			{
				if (font_data.wide_char_glyphs[c].texture_handle != 0)
				{
					characters.push_back(static_cast<wchar_t>(c));
					++native_count;
				}
			}

			console::info("[font] the built-in fonts cover %i characters\n", native_count);

			for (auto c = 0x20; c < 0x7F; c++)
			{
				characters.push_back(static_cast<wchar_t>(c));
			}

			const auto configured = parse_character_ranges(ranges);
			characters.insert(characters.end(), configured.begin(), configured.end());

			const auto common = common_chinese_characters(chinese);
			characters.insert(characters.end(), common.begin(), common.end());

			for (const auto character : utf8_to_wide(extra))
			{
				if (character > 0 && character <= 0xFFFF)
				{
					characters.push_back(character);
				}
			}

			std::sort(characters.begin(), characters.end());
			characters.erase(std::unique(characters.begin(), characters.end()), characters.end());

			// the characters the game could not draw before, which is what the
			// common chinese set is there to reduce
			auto missing_count = 0;
			for (const auto character : characters)
			{
				if (font_data.wide_char_glyphs[static_cast<unsigned int>(character)].texture_handle == 0)
				{
					++missing_count;
				}
			}

			console::info("[font] %i characters requested, %i of them the built-in fonts have no glyph for\n",
				static_cast<int>(characters.size()), missing_count);

			if (static_cast<int>(characters.size()) > ttf_max_characters)
			{
				console::info("[font] %i characters requested, only the first %i are loaded\n",
					static_cast<int>(characters.size()), ttf_max_characters);
				characters.resize(ttf_max_characters);
			}

			return characters;
		}

		// the box the built-in font of this character ended up with. taking it as the
		// target keeps the text layout the game calculates, so only the ink changes
		ttf_glyph_target_t native_target(const wchar_t character, const int spacing)
		{
			const auto& info = font_data.wide_char_glyphs[static_cast<unsigned int>(character)];
			if (info.texture_handle == 0)
			{
				return {};
			}

			return target_from_metrics(info.metrics, spacing);
		}

		// how far the metrics the game calculates differ from the ones the built-in
		// font had, in the same 60ths of the text height the records are written in
		struct ttf_record_delta_t
		{
			bool valid = false;
			int width = 0;
			int height = 0;
			int horizontal_shift = 0;
			int vertical_shift = 0;
			int advance = 0;
		};

		// box filters a rasterized glyph into the box the game expects, so the ink
		// fills its cell no matter what size gdi rendered it at
		void resample_bitmap(const ttf_glyph_bitmap_t& source, const int width, const int height, std::vector<unsigned char>& result)
		{
			result.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
			if (source.width <= 0 || source.height <= 0)
			{
				return;
			}

			for (auto y = 0; y < height; y++)
			{
				const auto first_y = y * source.height / height;
				const auto last_y = std::max(first_y + 1, (y + 1) * source.height / height);

				for (auto x = 0; x < width; x++)
				{
					const auto first_x = x * source.width / width;
					const auto last_x = std::max(first_x + 1, (x + 1) * source.width / width);

					auto covered = 0;
					auto total = 0;
					for (auto source_y = first_y; source_y < last_y; source_y++)
					{
						for (auto source_x = first_x; source_x < last_x; source_x++)
						{
							covered += source.pixels[static_cast<size_t>(source_y) * source.width + source_x];
							++total;
						}
					}

					if (covered * 2 >= total)
					{
						result[static_cast<size_t>(y) * width + x] = 1;
					}
				}
			}
		}

		// builds the box for a character out of the size the font itself draws it at.
		// the built-in fonts are deliberately not asked for it: taking their boxes over
		// is what made a kanji come out half again as tall as the latin beside it, and
		// that proportion belongs to the game's own fonts rather than to the one loaded
		// here. the reference character still places the glyph, so the baseline and the
		// left bearing stay where the game puts them
		ttf_glyph_target_t ink_target(const ttf_glyph_bitmap_t& bitmap, const font_template_t& reference,
			const ttf_glyph_target_t& reference_target, const float raster_scale, const float pixels_per_unit)
		{
			ttf_glyph_target_t target{};
			const auto to_units = [pixels_per_unit](const int pixels)
			{
				return static_cast<int>(std::lround(static_cast<float>(pixels) / pixels_per_unit));
			};

			// the reference was measured at ttf_reference_size while the character is
			// rasterized at the size the group is loaded with
			const auto reference_origin_x = to_units(static_cast<int>(std::lround(static_cast<float>(reference.reference_origin_x) * raster_scale)));
			const auto reference_origin_y = to_units(static_cast<int>(std::lround(static_cast<float>(reference.reference_origin_y) * raster_scale)));

			target.valid = true;
			target.width = to_units(bitmap.width);
			target.height = to_units(bitmap.height);
			target.horizontal_shift = reference_target.horizontal_shift + to_units(bitmap.origin_x) - reference_origin_x;
			target.vertical_shift = reference_target.vertical_shift + reference_origin_y - to_units(bitmap.origin_y);
			target.advance = to_units(bitmap.advance);
			return target;
		}

		struct ttf_glyph_t
		{
			wchar_t character = 0;
			ttf_glyph_target_t target;
			std::vector<unsigned char> pixels;
			int bitmap_x = 0;
			int bitmap_y = 0;
		};

		// rasterizes the requested characters and registers them group by group so
		// the game can hand out as many atlases as the glyphs need. every glyph keeps
		// the size the font draws it at, so a kanji, a kana and the latin beside them
		// come out in the proportion the font was designed with
		void load_ttf_glyphs(const HDC dc, const std::vector<wchar_t>& characters,
			const int raster_size, const font_template_t& fallback, const int font_spacing,
			const int shift_x, const int shift_y, const float pixels_per_unit)
		{
			// the reference ink was measured at ttf_reference_size, gdi rasterizes at
			// raster_size, so both have to be brought to the same scale first
			const auto raster_scale = static_cast<float>(raster_size) / static_cast<float>(ttf_reference_size);
			const auto reference_target = template_target(fallback, font_spacing);

			if (pixels_per_unit <= 0.f || !reference_target.valid)
			{
				console::info("[font] raster %ipx: no reference cell, the built-in glyphs stay\n", raster_size);
				return;
			}

			console::info("[font] atlas group: raster %ipx, 1 unit = %.3f px, reference 0x%X box %ix%i at %i/%i advance %i, reference ink %i at %i/%i at %ipx (%.3f)\n",
				raster_size, pixels_per_unit, fallback.character, reference_target.width, reference_target.height,
				reference_target.horizontal_shift, reference_target.vertical_shift, reference_target.advance,
				fallback.reference_ink, fallback.reference_origin_x, fallback.reference_origin_y,
				ttf_reference_size, raster_scale);

			std::vector<ttf_glyph_t> glyphs;
			glyphs.reserve(characters.size());

			std::vector<unsigned char> scratch;
			auto skipped = 0;

			// the game rewrites a glyph record on its way in, and how exactly it does
			// that is not worth guessing: register one glyph whose metrics are known,
			// read them back and take the difference as the correction for the rest
			auto delta = ttf_record_delta_t{};
			for (const auto character : characters)
			{
				const auto probe = native_target(character, font_spacing);
				if (!probe.valid || probe.width < 1 || probe.height < 1 || probe.advance < 1)
				{
					continue;
				}

				ttf_glyph_bitmap_t bitmap{};
				if (!rasterize_glyph(dc, character, scratch, bitmap) || bitmap.width <= 0 || bitmap.height <= 0)
				{
					continue;
				}

				const auto index = static_cast<unsigned int>(character);
				const auto before = font_data.wide_char_glyphs[index].metrics;

				std::vector<unsigned char> pixels;
				resample_bitmap(bitmap, probe.width, probe.height, pixels);

				std::vector<game::fox::gr::dg::FontData_Glyph> probe_records(1);
				probe_records[0].character = index;
				probe_records[0].xOffset = 0;
				probe_records[0].yOffset = 0;
				probe_records[0].width = static_cast<unsigned char>(std::clamp(probe.width, 1, 255));
				probe_records[0].height = static_cast<unsigned char>(std::clamp(probe.height, 1, 255));
				probe_records[0].layer = 0;
				probe_records[0].horizontalSpace = static_cast<unsigned char>(std::clamp(probe.advance, 1, 255));
				probe_records[0].horizontalShift = static_cast<char>(std::clamp(probe.horizontal_shift, -128, 127));
				probe_records[0].verticalShift = static_cast<char>(std::clamp(probe.vertical_shift, -128, 127));

				game::fox::gr::dg::FontData probe_font{};
				probe_font.spacing = static_cast<char>(font_spacing);
				probe_font.fontIndex = 0;
				probe_font.glyphCount = 1;
				probe_font.glyphs = probe_records.data();
				probe_font.fontDataBuffer = pixels.data();
				probe_font.width = static_cast<float>(probe.width);
				probe_font.height = static_cast<float>(probe.height);
				probe_font.unk1_3 = static_cast<unsigned char>(std::clamp(probe.height, 1, 255));

				load_font_internal(&probe_font, 0, 1, 1.f, 0, 0, 0);

				const auto after = font_data.wide_char_glyphs[index].metrics;
				const auto to_units = [](const float value) { return static_cast<int>(std::lround(value * 60.f)); };

				delta.width = to_units(after.f5) - to_units(before.f5);
				delta.height = to_units(after.f6) - to_units(before.f6);
				delta.horizontal_shift = to_units(after.f7) - to_units(before.f7);
				delta.vertical_shift = to_units(after.f8) - to_units(before.f8);
				delta.advance = to_units(after.f9) - to_units(before.f9);
				delta.valid = true;

				console::info("[font] 0x%X record %ix%i at %i/%i advance %i -> metrics box %.2f/%.2f, left %.2f, top %.2f, advance %.2f (built-in had %.2f/%.2f, %.2f, %.2f, %.2f)\n",
					index, probe.width, probe.height, probe.horizontal_shift, probe.vertical_shift, probe.advance,
					after.f5 * 60.f, after.f6 * 60.f, after.f7 * 60.f, after.f8 * 60.f, after.f9 * 60.f,
					before.f5 * 60.f, before.f6 * 60.f, before.f7 * 60.f, before.f8 * 60.f, before.f9 * 60.f);
				break;
			}

			if (delta.valid)
			{
				console::info("[font] record correction: %.2f/%.2f box, %.2f left, %.2f top, %.2f advance (units)\n",
					static_cast<float>(delta.width), static_cast<float>(delta.height),
					static_cast<float>(delta.horizontal_shift), static_cast<float>(delta.vertical_shift),
					static_cast<float>(delta.advance));
			}

			for (const auto character : characters)
			{
				// a character the ttf has no glyph for would come back as the notdef
				// box, which would replace a glyph the built-in fonts do have
				WORD glyph_index = 0;
				if (GetGlyphIndicesW(dc, &character, 1, &glyph_index, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR
					&& glyph_index == 0xFFFF)
				{
					++skipped;
					continue;
				}

				ttf_glyph_bitmap_t bitmap{};
				if (!rasterize_glyph(dc, character, scratch, bitmap) || bitmap.width <= 0 || bitmap.height <= 0)
				{
					// no ink, the character keeps the glyph the built-in font has for
					// it, which also keeps the advance of a space intact
					++skipped;
					continue;
				}

				auto target = ink_target(bitmap, fallback, reference_target, raster_scale, pixels_per_unit);

				if (!target.valid || target.width <= 0 || target.height <= 0 || target.advance <= 0)
				{
					++skipped;
					continue;
				}

				if (delta.valid)
				{
					// only the top moves against its record, so it is the one metric
					// the measured difference has to be added back for instead of
					// taken off, which is also what made the old sign so far off
					target.width -= delta.width;
					target.height -= delta.height;
					target.horizontal_shift -= delta.horizontal_shift;
					target.vertical_shift += delta.vertical_shift;
					target.advance -= delta.advance;
				}

				if (character == 0x4E2D || character == 0x30D3 || character == 0x304B || character == 0x3002 || character == 0x674E)
				{
					console::info("[font] 0x%X: ink %ix%i at %i/%i, advance %i -> box %ix%i at %i/%i, advance %i\n",
						character, bitmap.width, bitmap.height, bitmap.origin_x, bitmap.origin_y,
						bitmap.advance, target.width, target.height, target.horizontal_shift,
						target.vertical_shift, target.advance);
				}

				target.width = std::clamp(target.width, 1, 255);
				target.height = std::clamp(target.height, 1, 255);
				target.advance = std::clamp(target.advance, 1, 255);
				target.horizontal_shift += shift_x;
				target.vertical_shift += shift_y;

				ttf_glyph_t glyph{};
				glyph.character = character;
				glyph.target = target;
				resample_bitmap(bitmap, target.width, target.height, glyph.pixels);

				// a glyph with no set pixel draws as a blank cell, which looks exactly
				// like a character the game has no glyph for, so the ink is worth
				// saying out loud for the characters being watched
				if (character == 0x4E2D || character == 0x30D3 || character == 0x304B || character == 0x3002 || character == 0x674E)
				{
					const auto ink = std::count(glyph.pixels.begin(), glyph.pixels.end(), static_cast<unsigned char>(1));
					console::info("[font] 0x%X: %i of %i pixels set\n", character, static_cast<int>(ink),
						static_cast<int>(glyph.pixels.size()));
				}

				glyphs.push_back(std::move(glyph));
			}

			console::info("[font] %i characters: %i replaced, %i skipped\n",
				static_cast<int>(characters.size()), static_cast<int>(glyphs.size()), skipped);
			if (glyphs.empty())
			{
				return;
			}

			std::vector<game::fox::gr::dg::FontData_Glyph> records;
			records.reserve(glyphs.size());

			game::fox::gr::dg::FontData font{};
			font.spacing = static_cast<char>(font_spacing);
			font.fontIndex = 0;
			font.glyphs = records.data();

			std::vector<unsigned char> source(static_cast<size_t>(ttf_source_width) * ttf_source_height);

			size_t index = 0;
			while (index < glyphs.size())
			{
				std::fill(source.begin(), source.end(), 0);

				const auto batch_first = index;
				const auto batch_begin = static_cast<int>(records.size());
				auto cursor_x = 1;
				auto cursor_y = 1;
				auto row_height = 0;

				while (index < glyphs.size() && index - batch_first < ttf_max_batch)
				{
					auto& glyph = glyphs[index];
					const auto width = glyph.target.width;
					const auto height = glyph.target.height;

					if (cursor_x + width + 2 > ttf_source_width)
					{
						cursor_x = 1;
						cursor_y += row_height + 1;
						row_height = 0;
					}

					if (cursor_y + height + 2 > ttf_source_height)
					{
						break;
					}

					glyph.bitmap_x = cursor_x;
					glyph.bitmap_y = cursor_y;

					for (auto y = 0; y < height; y++)
					{
						std::memcpy(source.data() + static_cast<size_t>(cursor_y + y) * ttf_source_width + cursor_x,
							glyph.pixels.data() + static_cast<size_t>(y) * width, static_cast<size_t>(width));
					}

					cursor_x += width + 1;
					row_height = std::max(row_height, height);
					++index;
				}

				if (index == batch_first)
				{
					break;
				}

				auto cell_height = 0;
				const auto batch_last = index;

				for (auto i = batch_first; i < batch_last; i++)
				{
					const auto& glyph = glyphs[i];
					cell_height = std::max(cell_height, glyph.target.height);

					game::fox::gr::dg::FontData_Glyph record{};
					record.character = static_cast<unsigned int>(glyph.character);
					record.xOffset = static_cast<unsigned short>(glyph.bitmap_x);
					record.yOffset = static_cast<unsigned short>(glyph.bitmap_y);
					record.width = static_cast<unsigned char>(std::clamp(glyph.target.width, 1, 255));
					record.height = static_cast<unsigned char>(std::clamp(glyph.target.height, 1, 255));
					record.layer = 0;
					record.horizontalSpace = static_cast<unsigned char>(std::clamp(glyph.target.advance, 1, 255));
					record.horizontalShift = static_cast<char>(std::clamp(glyph.target.horizontal_shift, -128, 127));
					record.verticalShift = static_cast<char>(std::clamp(glyph.target.vertical_shift, -128, 127));
					records.push_back(record);
				}

				font.glyphCount = static_cast<short>(records.size());
				font.unk1_3 = static_cast<unsigned char>(cell_height);
				font.fontDataBuffer = source.data();
				font.width = static_cast<float>(ttf_source_width);
				font.height = static_cast<float>(ttf_source_height);

				const auto batch_count = static_cast<int>(records.size()) - batch_begin;
				auto done = 0;
				while (done < batch_count)
				{
					const auto loaded = load_font_internal(&font, batch_begin + done, batch_count - done, 1.f, 0, 0, 0);
					if (loaded <= 0)
					{
						break;
					}

					done += loaded;
				}

				console::info("[font] registered %i/%i glyphs\n", static_cast<int>(batch_last), static_cast<int>(glyphs.size()));
			}
		}

		void load_ttf_font()
		{
			const auto file = r_font_file->current.get_string();
			if (file.empty())
			{
				return;
			}

			const auto name = utf8_to_wide(r_font_name->current.get_string());
			const auto files = find_font_files(utf8_to_wide(file), name);

			if (files.empty())
			{
				console::info("[font] no ttf/otf file found for \"%s\"\n", file.data());
				return;
			}

			const auto dc = CreateCompatibleDC(nullptr);

			std::wstring family;
			auto registered = std::filesystem::path{};
			for (const auto& candidate : files)
			{
				if (add_private_font_file(candidate, dc, name, family))
				{
					registered = candidate;
					break;
				}
			}

			if (registered.empty())
			{
				DeleteDC(dc);
				return;
			}

			const auto old_font = static_cast<HFONT>(GetCurrentObject(dc, OBJ_FONT));
			const auto _0 = gsl::finally([&]
			{
				RemoveFontResourceExW(registered.wstring().c_str(), FR_PRIVATE, nullptr);
				SelectObject(dc, old_font);
				DeleteDC(dc);
			});

			const auto scale = std::max(0.1f, r_font_scale->current.get_float());
			const auto configured_size = r_font_size->current.get_int();

			const wchar_t kanji_samples[] = {static_cast<wchar_t>(0x4E2D), static_cast<wchar_t>(0x56FD), static_cast<wchar_t>(0x65E5), static_cast<wchar_t>(0x76EE)};
			const wchar_t latin_samples[] = {L'H', L'E', L'B', L'0'};

			auto measure_font = create_gdi_font(family, ttf_reference_size);
			SelectObject(dc, measure_font);
			const auto kanji_reference = measure_reference_glyph(dc, kanji_samples, 4);
			const auto latin_reference = measure_reference_glyph(dc, latin_samples, 4);
			SelectObject(dc, old_font);
			DeleteObject(measure_font);

			if (kanji_reference.height <= 0 && latin_reference.height <= 0)
			{
				console::info("[font] ttf \"%s\" has no usable glyphs\n", wide_to_utf8(family).data());
				return;
			}

			// every built-in font is in by now, so this is the table the game will
			// draw its own glyphs from and the one the replacement glyphs have to
			// match
			capture_font_template(font_template_role::kanji);
			capture_font_template(font_template_role::latin);

			// the boxes the built-in fonts give these characters, to read the sizes the
			// replacement ends up with (printed at the end) against
			for (const auto character : {0x4E2D, 0x65E5, 0x30D3, 0x48, 0x30})
			{
				const auto native = native_target(static_cast<wchar_t>(character), ttf_font_spacing);
				if (native.valid)
				{
					console::info("[font] built-in 0x%X: %ix%i at %i/%i advance %i\n", character,
						native.width, native.height, native.horizontal_shift, native.vertical_shift, native.advance);
				}
			}

			kanji_font_template.reference_ink = kanji_reference.height;
			kanji_font_template.reference_origin_x = kanji_reference.origin_x;
			kanji_font_template.reference_origin_y = kanji_reference.origin_y;
			latin_font_template.reference_ink = latin_reference.height;
			latin_font_template.reference_origin_x = latin_reference.origin_x;
			latin_font_template.reference_origin_y = latin_reference.origin_y;

			// the size the latin reference covers in the built-in font is what sets how
			// large everything is drawn
			const auto raster_size_for = [&](const font_template_t& reference, const ttf_glyph_bitmap_t& ink)
			{
				if (!reference.valid || reference.reference_ink <= 0 || ink.height <= 0)
				{
					return 0;
				}

				return static_cast<int>(
					ttf_reference_size * template_target(reference, ttf_font_spacing).height * scale / reference.reference_ink + 0.5f);
			};

			const auto kanji_group_scale = std::max(0.2f, r_font_kanji_scale->current.get_float());
			const auto latin_group_scale = std::max(0.2f, r_font_latin_scale->current.get_float());

			// one raster size for both groups, and the size a character ends up with is
			// the size the font itself draws it at. rasterizing kana at a different size
			// than latin is what let them drift apart: the two only keep the proportion
			// the font was designed with if they are drawn at the same size, and only
			// the latin one is fixed, so latin text keeps the size it already had
			auto latin_size = configured_size > 0
				? configured_size
				: static_cast<int>(raster_size_for(latin_font_template, latin_reference) * latin_group_scale + 0.5f);
			if (latin_size <= 0)
			{
				latin_size = static_cast<int>(18 * scale + 0.5f);
			}

			const auto kanji_size = std::max(1, static_cast<int>(static_cast<float>(latin_size) * kanji_group_scale + 0.5f));

			// what one bitmap pixel is worth in the units the game measures with, taken
			// off the latin reference: its ink covers the box the built-in font gives
			// that character, so a glyph with the same ink as the built-in's comes out
			// the size it already was
			const auto latin_raster_scale = static_cast<float>(latin_size) / static_cast<float>(ttf_reference_size);
			const auto latin_reference_target = template_target(latin_font_template, ttf_font_spacing);
			const auto pixels_per_unit = latin_font_template.valid && latin_font_template.reference_ink > 0 && latin_reference_target.height > 0
				? static_cast<float>(latin_font_template.reference_ink) * latin_raster_scale / static_cast<float>(latin_reference_target.height)
				: 0.f;

			console::info("[font] ttf %s: raster %ipx (kanji %.2f, latin %.2f), 1 unit = %.3f px\n",
				wide_to_utf8(family).data(), latin_size, kanji_group_scale, latin_group_scale, pixels_per_unit);

			const auto characters = collect_ttf_characters(r_font_ranges->current.get_string(), r_font_extra->current.get_string(),
				r_font_chinese->current.get_int());

			auto shift_x = r_font_x_offset->current.get_int();
			auto shift_y = r_font_y_offset->current.get_int();
			if (shift_x == 999) shift_x = 0;
			if (shift_y == 999) shift_y = 0;

			const auto latin_last = std::lower_bound(characters.begin(), characters.end(), static_cast<wchar_t>(0x2000));
			const std::vector<wchar_t> latin_characters(characters.begin(), latin_last);
			const std::vector<wchar_t> kanji_characters(latin_last, characters.end());

			if (!latin_characters.empty())
			{
				auto group_font = create_gdi_font(family, latin_size);
				SelectObject(dc, group_font);
				load_ttf_glyphs(dc, latin_characters, latin_size, latin_font_template, ttf_font_spacing, shift_x, shift_y, pixels_per_unit);
				SelectObject(dc, old_font);
				DeleteObject(group_font);
			}

			if (!kanji_characters.empty())
			{
				auto group_font = create_gdi_font(family, kanji_size);
				SelectObject(dc, group_font);
				load_ttf_glyphs(dc, kanji_characters, kanji_size, kanji_font_template, ttf_font_spacing, shift_x, shift_y, pixels_per_unit);
				SelectObject(dc, old_font);
				DeleteObject(group_font);
			}

			// the sizes that ended up in the table, so what the replacement came out at
			// can be read off the log instead of having to be measured on screen: 中 and
			// び are characters the built-in fonts have, 练 and 鹅 are ones only the ttf
			// has
			for (const auto character : {0x4E2D, 0x7EC3, 0x9E45, 0x674E, 0x30D3, 0x48, 0x30})
			{
				const auto& info = font_data.wide_char_glyphs[static_cast<unsigned int>(character)];
				console::info("[font] check 0x%X: %.2f x %.2f, left %.2f, top %.2f, advance %.2f (texture %i)\n",
					character, info.metrics.f5 * 60.f, info.metrics.f6 * 60.f, info.metrics.f7 * 60.f,
					info.metrics.f8 * 60.f, info.metrics.f9 * 60.f, info.texture_handle != 0 ? 1 : 0);
			}

			// how much taller a kanji came out than the latin beside it, which is the
			// proportion this path exists to get right
			{
				const auto height_of = [](const unsigned int character)
				{
					return font_data.wide_char_glyphs[character].metrics.f6 * 60.f;
				};

				const auto kanji_height = height_of(0x4E2D);
				const auto latin_height = std::max(1.f, std::max(height_of(0x48), height_of(0x30)));
				console::info("[font] ratio: kanji/latin %.2f (%.1f vs %.1f), kana/kanji %.2f\n",
					kanji_height / latin_height, kanji_height, latin_height,
					height_of(0x30D3) / std::max(1.f, kanji_height));
			}
		}

		void init_fonts()
		{
			// the reference glyph has to be read back once every built-in font is in:
			// they all register into the same table, so an earlier font's metrics for
			// a character a later one also has are overwritten and would give the
			// replacement glyphs a box nothing on screen has
			load_font("/Assets/tpp/font/KanjiFont.ffnt", kanji_font_scale, kanji_font_spacing, -8, 5);
			load_font("/Assets/tpp/font/font_def_jp.ffnt", 1.3f, 4, 0, 5);
			load_font("/Assets/tpp/font/russian.ffnt", 1.0f, -20, -8, 16);
			load_font("/Assets/tpp/font/font_def_ltn.ffnt", latin_font_scale, latin_font_spacing, 0, 0);

			// loaded last, so they override the glyphs of the fonts above
			for (const auto& var : {r_extra_font_1, r_extra_font_2})
			{
				const auto& font_name = var->current.get_string();
				if (!font_name.empty())
				{
					load_font(font_name.data(), 1.2f, -6, -8, 5);
				}
			}

			load_ttf_font();
		}

		bool init_font_data()
		{
			const auto font_system = *game::fox::gr::dg::FontSystem_::m_instance;
			if (!font_system)
			{
				return false;
			}

			const auto font_type = get_font_type();

			auto ascii_count = 0;
			unsigned char ascii_chars[255]{};

			for (auto i = 32; i < 127; i++)
			{
				ascii_chars[ascii_count++] = static_cast<unsigned char>(i);
			}

			for (auto i = 160; i < 254; i++)
			{
				ascii_chars[ascii_count++] = static_cast<unsigned char>(i);
			}

			auto utf8_index = 0;
			char utf8_chars[255 * 4]{};

			for (auto i = 0; i < ascii_count; i++)
			{
				const auto c = ascii_chars[i];
				if (c < 128)
				{
					utf8_chars[utf8_index++] = static_cast<char>(c);
				}
				else
				{
					utf8_chars[utf8_index++] = static_cast<char>(0xC0 | (c >> 6));
					utf8_chars[utf8_index++] = static_cast<char>(0x80 | (c & 0x3F));
				}
			}

			game::fox::gr::dg::FontSystem_::RegisterString(font_system, utf8_chars, font_type);
			game::fox::gr::dg::StringFontMetricsCache metrics{};
			game::fox::gr::InitMetrics(&metrics, utf8_chars, 1, font_type);

			if (metrics.count <= 0)
			{
				return false;
			}

			const auto glyph_data = game::fox::gr::dg::FontSystem_::GetFontTextureGlyphDataTop(font_system);

			for (auto i = 0; i < metrics.count; i++)
			{
				const auto idx = static_cast<unsigned char>(ascii_chars[i]);
				std::memcpy(&font_data.system_font_glyphs[idx], &glyph_data[metrics.glyphs[i]], sizeof(game::fox::gr::dg::_TextureGlyphData));
			}

			const auto font_manager = *game::fox::ui::RawDaemon_::s_fontManager;
			if (font_manager == nullptr)
			{
				return false;
			}

			game::fox::StringId language_code{};
			language_code.id = 0xEF76460534C5; // "eng"

			game::fox::StringId font_name{};
			font_name.id = 0x1829B3B18A01; // "cmn-hud-art-L"

			const auto font_group = game::fox::ui::FontManager_::GetFontGroup(font_manager, language_code, font_name.f.l);
			if (font_group == nullptr || font_group->font == nullptr || font_group->font->texture == nullptr)
			{
				return false;
			}

			font_data.artist_font_texture = font_group->font->texture;

			auto res_width = 0.f;
			auto res_height = 0.f;

			static game::Vectormath::Aos::Vector2 xy[0x200]{};
			static game::Vectormath::Aos::Vector2 uv[0x200]{};

			font_data.artist_font_height = 50.f;

			const auto chars = " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
			if (!game::fox::ui::Font_::CreateText(font_group->font, chars, font_data.artist_font_height, font_data.artist_font_height, font_group->spacing,
				xy, uv, &res_width, &res_height))
			{
				return false;
			}

			auto previous_x = 0.f;
			for (auto i = 0; i < 95; i++)
			{
				const auto x = xy[(i * 4) + 3].values[0];
				const auto c = chars[i];

				const auto target_width = font_data.artist_font_height / 2.f;
				const auto width = xy[(i * 4) + 3].values[0] - xy[(i * 4) + 0].values[0];
				auto diff = (target_width - width) / 2.f;
				if (!std::isalnum(c))
				{
					diff = 0.f;
				}

				font_data.artist_font_gylphs[c].xy[0][0] = xy[(i * 4) + 0].values[0] - previous_x + diff;
				font_data.artist_font_gylphs[c].xy[1][0] = xy[(i * 4) + 1].values[0] - previous_x + diff;
				font_data.artist_font_gylphs[c].xy[2][0] = xy[(i * 4) + 2].values[0] - previous_x + diff;
				font_data.artist_font_gylphs[c].xy[3][0] = xy[(i * 4) + 3].values[0] - previous_x + diff;

				font_data.artist_font_gylphs[c].xy[0][1] = xy[(i * 4) + 0].values[1];
				font_data.artist_font_gylphs[c].xy[1][1] = xy[(i * 4) + 1].values[1];
				font_data.artist_font_gylphs[c].xy[2][1] = xy[(i * 4) + 2].values[1];
				font_data.artist_font_gylphs[c].xy[3][1] = xy[(i * 4) + 3].values[1];

				font_data.artist_font_gylphs[c].uv[0][0] = uv[(i * 4) + 0].values[0];
				font_data.artist_font_gylphs[c].uv[1][0] = uv[(i * 4) + 1].values[0];
				font_data.artist_font_gylphs[c].uv[2][0] = uv[(i * 4) + 2].values[0];
				font_data.artist_font_gylphs[c].uv[3][0] = uv[(i * 4) + 3].values[0];

				font_data.artist_font_gylphs[c].uv[0][1] = uv[(i * 4) + 0].values[1];
				font_data.artist_font_gylphs[c].uv[1][1] = uv[(i * 4) + 1].values[1];
				font_data.artist_font_gylphs[c].uv[2][1] = uv[(i * 4) + 2].values[1];
				font_data.artist_font_gylphs[c].uv[3][1] = uv[(i * 4) + 3].values[1];

				font_data.artist_font_gylphs[c].width = target_width;
				previous_x = x;
			}

			return true;
		}

		utils::hook::detour change_language_hook;
		void change_language_stub(void* a1)
		{
			change_language_hook.invoke<void>(a1);
			font_data.loaded = init_font_data();
		}

		void* alloc_font_metrics_stub(int a1, int a2)
		{
			const auto buffer = game::fox::KernelAllocAligned(sizeof(custom_font_metrics_t), 4);
			std::memset(buffer, 0, sizeof(custom_font_metrics_t));
			return buffer;
		}

		void free_font_metrics_stub(custom_font_metrics_t* metrics)
		{
			if (metrics->string != nullptr)
			{
				game::fox::FreeAnnotated(metrics->string, 0x5000F);
				metrics->string = nullptr;
			}
			
			game::fox::FreeAnnotated(metrics, 0x5000F);
		}

		void init_metrics_stub(custom_font_metrics_t* string, const char* text, int a3, int font_type)
		{
			game::fox::gr::InitMetrics(&string->native, text, a3, font_type);

			if (!r_custom_text_rendering->current.enabled() || !is_system_font(font_type))
			{
				return;
			}

			auto count = 0;
			{
				auto c = reinterpret_cast<unsigned char*>(reinterpret_cast<size_t>(text));
				while (*c != 0)
				{
					game::fox::Utf8ToUtf32(&c);
					++count;
				};
			}

			auto buffer = game::fox::KernelAllocAligned(count * 2, 4);
			string->string = reinterpret_cast<wchar_t*>(buffer);

			auto c = reinterpret_cast<unsigned char*>(reinterpret_cast<size_t>(text));
			auto idx = 0;
			do
			{
				string->string[idx++] = static_cast<wchar_t>(game::fox::Utf8ToUtf32(&c));
			} while (*c != 0);

			string->str_len = count;
		}

		utils::hook::detour get_string_width_hook;
		int get_string_width_stub(void* a1, game::fox::gr::dg::FontData* a2, unsigned char* text, float* length, char font_type)
		{
			if (!r_custom_text_rendering->current.enabled() || !is_system_font(a2->fontIndex))
			{
				return get_string_width_hook.invoke<int>(a1, a2, text, length, font_type);
			}

			auto c = text;
			auto idx = 0;
			*length = 0.f;

			auto spacing = 0.f;
			auto scaling = 1.f;
			switch (get_language_code())
			{
			case 'npj':
				spacing = 3.f;
				scaling = 1.f;
				break;
			case 'sur':
				scaling = 1.1f;
				spacing = 0.f;
				break;
			default:
				spacing = 3.f;
				scaling = 1.f;
				break;
			}

			while (*c != 0)
			{
				if (c[0] == '^' && get_color_code(static_cast<char>(c[1]), nullptr))
				{
					c += 2;
					continue;
				}

				const auto char_idx = game::fox::Utf8ToUtf32(&c);
				if (char_idx >= 0xFFFF || !is_char_printable(static_cast<wchar_t>(char_idx)))
				{
					continue;
				}

				*length += font_data.wide_char_glyphs[char_idx].metrics.f9 * scaling + spacing * (1.f / 60.f);
				++idx;
			}

			return idx;
		}

		utils::hook::detour get_utf8_count_hook;
		int get_utf8_count_stub(unsigned char* text)
		{
			if (!r_custom_text_rendering->current.enabled())
			{
				return get_utf8_count_hook.invoke<int>(text);
			}

			auto c = text;
			auto idx = 0;
			while (*c != 0)
			{
				if (c[0] == '^' && get_color_code(static_cast<char>(c[1]), nullptr))
				{
					c += 2;
					continue;
				}

				const auto char_idx = game::fox::Utf8ToUtf32(&c);
				if (char_idx >= 0xFFFF || !is_char_printable(static_cast<wchar_t>(char_idx)))
				{
					continue;
				}

				++idx;
			}

			return idx;
		}
	}

	bool is_char_printable(const char c)
	{
		auto idx = static_cast<unsigned char>(c);
		return font_data.system_font_glyphs[idx].character != 0;
	}

	bool is_char_printable(const wchar_t c)
	{
		return iswprint(c) && font_data.wide_char_glyphs[c].texture_handle != 0;
	}

	bool is_system_font(int font_type)
	{
		return font_type == 1 || font_type == 2;
	}

	namespace fonts
	{
		class component final : public component_interface
		{
		public:
			void pre_load() override
			{
				r_extra_font_1 = vars::register_string("r_extra_font_1", "", vars::var_flag_saved,
					"extra font file to load after the built-in fonts, e.g. /Assets/tpp/font/full_1.ffnt (restart to apply)");
				r_extra_font_2 = vars::register_string("r_extra_font_2", "", vars::var_flag_saved,
					"second extra font file to load after r_extra_font_1");

				r_font_file = vars::register_string("r_font_file", "", vars::var_flag_saved,
					"ttf/otf font file or folder to render the game text with, e.g. C:\\Windows\\Fonts or C:\\Windows\\Fonts\\simhei.ttf (restart to apply)");
				r_font_name = vars::register_string("r_font_name", "", vars::var_flag_saved,
					"when r_font_file is a folder: part of the file name to pick, otherwise part of the family name");
				r_font_size = vars::register_int("r_font_size", 0, 0, 96, vars::var_flag_saved,
					"pixel size to rasterize r_font_file at, 0 = match the size of the game's own fonts");
				r_font_scale = vars::register_float("r_font_scale", 1.f, 0.2f, 4.f, vars::var_flag_saved,
					"size multiplier for r_font_file, 1 = same size as the game's own fonts");
				r_font_x_offset = vars::register_int("r_font_x_offset", 999, -64, 999, vars::var_flag_saved,
					"horizontal shift of the font in 60ths of the text height, 999 = automatic");
				r_font_y_offset = vars::register_int("r_font_y_offset", 999, -64, 999, vars::var_flag_saved,
					"vertical shift of the font in 60ths of the text height, 999 = automatic");
				r_font_ranges = vars::register_string("r_font_ranges", "0x3000-0x30FF,0xFF01-0xFF5E,0x2018-0x201D",
					vars::var_flag_saved, "comma separated character ranges to rasterize, e.g. 0x4E00-0x9FA5,0x3040-0x30FF");
				r_font_extra = vars::register_string("r_font_extra", "", vars::var_flag_saved,
					"extra characters to rasterize from r_font_file");
				r_font_chinese = vars::register_int("r_font_chinese", 1, 0, 2, vars::var_flag_saved,
					"how much of the common chinese set to rasterize from r_font_file as well, 0 = none, 1 = gb2312 level 1 (3755), 2 = level 1 and 2 (6763)");
				r_font_kanji_scale = vars::register_float("r_font_kanji_scale", 1.f, 0.2f, 2.f, vars::var_flag_saved,
					"size of the kanji, kana and chinese glyphs of r_font_file relative to r_font_latin_scale, which is the size the latin glyphs are drawn at. both groups are drawn at the same size by default, which is the proportion the font was designed with");
				r_font_latin_scale = vars::register_float("r_font_latin_scale", 1.f, 0.2f, 2.f, vars::var_flag_saved,
					"size of the latin glyphs of r_font_file. this one sets the size of the whole font, so it is the one to change if the text is too large or too small overall");
			}

			void start() override
			{
				if (game::environment::is_dedi())
				{
					return;
				}

				scheduler::once(init_fonts, scheduler::main);

				utils::hook::call(SELECT_VALUE(0x1402AF515, 0x140BA5CE5, 0x0, 0x0), alloc_font_metrics_stub);
				utils::hook::call(SELECT_VALUE(0x1402AF3C8, 0x140BA5B98, 0x0, 0x0), free_font_metrics_stub);
				utils::hook::call(SELECT_VALUE(0x1402AF52A, 0x140BA5CFA, 0x0, 0x0), init_metrics_stub);
				get_utf8_count_hook.create(SELECT_VALUE(0x140914AA0, 0x140686B60, 0x0, 0x0), get_utf8_count_stub);
				get_string_width_hook.create(SELECT_VALUE(0x140224650, 0x140B2A440, 0x0, 0x0), get_string_width_stub);
				change_language_hook.create(SELECT_VALUE(0x14090FA70, 0x140681A80, 0x0, 0x0), change_language_stub);
			}
		};
	}
}

REGISTER_COMPONENT(renderer::fonts::component)
