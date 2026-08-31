# Generate a client interface translation unit in the build tree with the
# legacy isometric in-map renderer and world-click controls redirected to the
# FPS renderer/input layer.
#
# We deliberately do this at configure time instead of hand-editing the very
# large ClientInterface.cpp file. All menus/inventory/dialog code remains
# upstream-identical; only gameplay entry points are intercepted.

function( fps_find_function_bounds source_text signature start_var end_var brace_var )
    string( FIND "${source_text}" "${signature}" function_pos )
    if( function_pos EQUAL -1 )
        message( FATAL_ERROR "Unable to find ${signature} in ClientInterface.cpp" )
    endif()

    string( SUBSTRING "${source_text}" ${function_pos} -1 function_tail )
    string( FIND "${function_tail}" "{" brace_rel )
    if( brace_rel EQUAL -1 )
        message( FATAL_ERROR "Unable to find opening brace for ${signature}" )
    endif()

    math( EXPR brace_pos "${function_pos} + ${brace_rel}" )
    string( LENGTH "${source_text}" source_len )
    set( cursor ${brace_pos} )
    set( depth 0 )
    set( function_end -1 )

    while( cursor LESS source_len )
        string( SUBSTRING "${source_text}" ${cursor} 1 current_char )
        if( current_char STREQUAL "{" )
            math( EXPR depth "${depth} + 1" )
        elseif( current_char STREQUAL "}" )
            math( EXPR depth "${depth} - 1" )
            if( depth EQUAL 0 )
                set( function_end ${cursor} )
                break()
            endif()
        endif()
        math( EXPR cursor "${cursor} + 1" )
    endwhile()

    if( function_end EQUAL -1 )
        message( FATAL_ERROR "Unable to find end of ${signature}" )
    endif()

    set( ${start_var} ${function_pos} PARENT_SCOPE )
    set( ${end_var} ${function_end} PARENT_SCOPE )
    set( ${brace_var} ${brace_pos} PARENT_SCOPE )
endfunction()

function( fps_replace_function text_var signature replacement_text )
    set( source_text "${${text_var}}" )
    fps_find_function_bounds( "${source_text}" "${signature}" function_pos function_end brace_pos )
    string( LENGTH "${source_text}" source_len )
    string( SUBSTRING "${source_text}" 0 ${function_pos} source_prefix )
    math( EXPR suffix_pos "${function_end} + 1" )
    math( EXPR suffix_len "${source_len} - ${suffix_pos}" )
    string( SUBSTRING "${source_text}" ${suffix_pos} ${suffix_len} source_suffix )
    set( ${text_var} "${source_prefix}${replacement_text}${source_suffix}" PARENT_SCOPE )
endfunction()

function( fps_inject_function_guard text_var signature guard_text )
    set( source_text "${${text_var}}" )
    fps_find_function_bounds( "${source_text}" "${signature}" function_pos function_end brace_pos )
    string( LENGTH "${source_text}" source_len )
    math( EXPR insert_pos "${brace_pos} + 1" )
    string( SUBSTRING "${source_text}" 0 ${insert_pos} source_prefix )
    math( EXPR suffix_len "${source_len} - ${insert_pos}" )
    string( SUBSTRING "${source_text}" ${insert_pos} ${suffix_len} source_suffix )
    set( ${text_var} "${source_prefix}${guard_text}${source_suffix}" PARENT_SCOPE )
endfunction()

function( fps_patch_client_interface input_file output_file )
    if( NOT EXISTS "${input_file}" )
        message( FATAL_ERROR "FPS patch input does not exist: ${input_file}" )
    endif()

    file( READ "${input_file}" source_text )

    fps_replace_function(
        source_text
        "void FOClient::GameDraw()"
        "void FOClient::GameDraw()\n{\n    FpsRenderer::DrawGame( *this );\n}\n" )

    # Suppress the old A-key weapon cursor and consume E while FPS gameplay is
    # captured. Key state itself is still maintained by ParseKeyboard, so WASD
    # remains continuous instead of becoming one action per key-repeat.
    fps_inject_function_guard(
        source_text
        "void FOClient::GameKeyDown( uchar dik, const char* dik_text )"
        "\n    if( FpsRenderer::KeyDown( *this, dik ) )\n        return;\n" )

    # World clicks become FPS fire/aim while an actual UI screen is not open.
    # FpsRenderer returns false outside FPS capture so legacy UI behavior stays
    # available wherever the original client expects it.
    fps_inject_function_guard(
        source_text
        "void FOClient::GameLMouseDown()"
        "\n    if( FpsRenderer::MouseButton( *this, 0, true ) )\n        return;\n" )
    fps_inject_function_guard(
        source_text
        "void FOClient::GameLMouseUp()"
        "\n    if( FpsRenderer::MouseButton( *this, 0, false ) )\n        return;\n" )
    fps_inject_function_guard(
        source_text
        "void FOClient::GameRMouseDown()"
        "\n    if( FpsRenderer::MouseButton( *this, 1, true ) )\n        return;\n" )
    fps_inject_function_guard(
        source_text
        "void FOClient::GameRMouseUp()"
        "\n    if( FpsRenderer::MouseButton( *this, 1, false ) )\n        return;\n" )

    # FpsRenderer is prepended before Core.h/Client.h. Its header only
    # forward-declares FOClient and uses built-in parameter types.
    file( WRITE "${output_file}"
        "#include \"FpsRenderer.h\"\n${source_text}" )

    message( STATUS "Generated FPS-only client interface: ${output_file}" )
endfunction()
