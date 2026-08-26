// FontStack implementation. See fontstack.h.

#include "fontstack.h"

#include "resourcestore.h"
#include "platform/respath.h"

#include <cairo/cairo-ft.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdio>
#include <utility>
#include <vector>

namespace Rations
{
namespace
{

//------------------------------------------------------------------------
// Everything a cairo font face needs to keep alive for as long as CAIRO says,
// not for as long as the FontStack that created it says. Attached to the face
// as user data, so cairo frees it when it drops its last internal reference.
//
// This is not belt-and-braces. cairo caches font faces and scaled fonts in
// process-wide tables, and the ft backend keys its cache on the FT_Face
// POINTER (cairo-ft-font.c, _cairo_ft_unscaled_font_keys_equal), holding the
// entry alive after cairo_font_face_destroy() returns. Calling FT_Done_Face()
// there — as this class used to — leaves that cache entry pointing at freed
// memory, and the next FT_New_Memory_Face() to be handed the same address (a
// second plug-in window, or the same one reopened) silently inherits the dead
// entry: its cached cairo font face, its scaled fonts and its already
// rasterised glyphs. The visible result is text drawn with another face's or
// another size's glyphs — a few characters at a time, intermittently, until
// the editor is recreated at a different address. cairo documents the rule on
// cairo_ft_font_face_create_for_ft_face(): "You must not call FT_Done_Face()
// before the last reference to the cairo_font_face_t has been dropped", and
// prescribes exactly this user-data callback as the way to obey it.
struct FaceOwner {
    FT_Face face = nullptr;
    std::vector<unsigned char> bytes; // empty for a built-in face (static storage)
    std::shared_ptr<void> library;    // FT_Library, released after the face
};

const cairo_user_data_key_t kFaceOwnerKey = {};

void destroyFaceOwner(void *data)
{
    FaceOwner *owner = static_cast<FaceOwner *>(data);
    if (owner->face)
        FT_Done_Face(owner->face);
    // `bytes` and the library reference go with the struct, in that order: the
    // face is gone before the memory it was reading and the library that owns
    // its internals.
    delete owner;
}

} // namespace

//------------------------------------------------------------------------
FontStack::~FontStack()
{
    // Only the references THIS object took are dropped here. The FT_Faces, the
    // font bytes and the FreeType library are released by destroyFaceOwner()
    // when cairo has finished with them, which can be later than this and is
    // cairo's decision to make (see FaceOwner).
    if (mTitle)
        cairo_font_face_destroy(mTitle);
    if (mBody)
        cairo_font_face_destroy(mBody);
}

//------------------------------------------------------------------------
cairo_font_face_t *FontStack::loadFace(const std::string &path, const std::string &rel, bool bold,
                                       void **outFt)
{
    *outFt = nullptr;

    std::vector<unsigned char> store;
    FT_Face face = nullptr;
    if (mLibrary) {
        // FT_New_Memory_Face over bytes we read, not FT_New_Face(path): the path-taking form
        // fopen()s the narrow string, which is code-page-limited on Windows, so a bundle installed
        // under a non-ASCII path would lose its fonts (platform/respath.h). FT_New_Memory_Face
        // does not copy, so `store` is moved into the FaceOwner below and lives as long as the
        // face does.
        if (readFileBytes(path, store)) {
            if (FT_New_Memory_Face(static_cast<FT_Library>(mLibrary.get()), store.data(),
                                   static_cast<FT_Long>(store.size()), 0, &face) != 0)
                face = nullptr;
        }
        if (!face)
            store.clear();

        // The built-in copy, for a binary with no fonts directory beside it. Its bytes are
        // required to have static storage duration (resourcestore.h), for the same reason.
        if (!face) {
            if (const EmbeddedResource *res = findEmbeddedResource(rel)) {
                if (FT_New_Memory_Face(static_cast<FT_Library>(mLibrary.get()), res->data,
                                       static_cast<FT_Long>(res->size), 0, &face) != 0)
                    face = nullptr;
            }
        }
    }

    if (face) {
        cairo_font_face_t *cf = cairo_ft_font_face_create_for_ft_face(face, 0);
        if (cf && cairo_font_face_status(cf) == CAIRO_STATUS_SUCCESS) {
            // Hand the face, its bytes and a library reference to cairo BEFORE the face is used
            // for anything, and treat a failure to do so as a failed load: a face cairo cannot be
            // made to own is a face nobody can safely free.
            FaceOwner *owner = new FaceOwner{face, std::move(store), mLibrary};
            if (cairo_font_face_set_user_data(cf, &kFaceOwnerKey, owner, destroyFaceOwner) ==
                CAIRO_STATUS_SUCCESS) {
                *outFt = face;
                return cf;
            }
            // cairo did not take the user data, so it will never run the callback. Take the bytes
            // back first: they have to still be there while FT_Done_Face() closes the stream over
            // them, and the face is destroyed below.
            store = std::move(owner->bytes);
            owner->face = nullptr;
            delete owner;
        }
        if (cf)
            cairo_font_face_destroy(cf);
        FT_Done_Face(face);
    }

    // Fallback: a generic toy face, so the editor still shows readable text.
    fprintf(stderr, "Rations: could not load font %s (using a system fallback)\n", path.c_str());
    return cairo_toy_font_face_create("sans-serif", CAIRO_FONT_SLANT_NORMAL,
                                      bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
}

//------------------------------------------------------------------------
bool FontStack::load(const std::string &resourceDir)
{
    FT_Library lib = nullptr;
    if (FT_Init_FreeType(&lib) == 0)
        mLibrary = std::shared_ptr<void>(
            lib, [](void *p) { FT_Done_FreeType(static_cast<FT_Library>(p)); });
    else
        fprintf(stderr, "Rations: FreeType failed to initialise (using system fallbacks)\n");

    const std::string dir = resourceDir + "/";
    mTitle =
        loadFace(dir + "fonts/Michroma-Regular.ttf", "fonts/Michroma-Regular.ttf", true, &mTitleFt);
    mBody = loadFace(dir + "fonts/Roboto-Regular.ttf", "fonts/Roboto-Regular.ttf", false, &mBodyFt);

    return mTitleFt != nullptr && mBodyFt != nullptr;
}

} // namespace Rations
