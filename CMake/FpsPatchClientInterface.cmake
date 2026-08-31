# Generate a client interface translation unit in the build tree with the
# legacy isometric FOClient::GameDraw() body replaced by the FPS renderer.
#
# We deliberately do this at configure time instead of editing the large
# legacy ClientInterface.cpp file. All of its menus/inventory/dialog code
# remains untouched; only the in-map drawing function is replaced.

function( fps_patch_client_interface input_file output_file )
    if( NOT EXISTS "${input_file}" )
        message( FATAL_ERROR "FPS patch input does not exist: ${input_file}" )
    endif()

    file( READ "${input_file}" source_text )

    set( signature "void FOClient::GameDraw()" )
    string( FIND "${source_text}" "${signature}" function_pos )
    if( function_pos EQUAL -1 )
        message( FATAL_ERROR "Unable to find ${signature} in ${input_file}" )
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

    # GameDraw contains ordinary C++ blocks. Counting braces gives us a
    # stable replacement even when upstream line numbers change.
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

    string( SUBSTRING "${source_text}" 0 ${function_pos} source_prefix )

    math( EXPR suffix_pos "${function_end} + 1" )
    math( EXPR suffix_len "${source_len} - ${suffix_pos}" )
    string( SUBSTRING "${source_text}" ${suffix_pos} ${suffix_len} source_suffix )

    set( fps_game_draw
"void FOClient::GameDraw()\n{\n    FpsRenderer::DrawGame( *this );\n}\n" )

    # FpsRenderer is prepended before Core.h/Client.h. Its header only
    # forward-declares FOClient, so this is safe for both DX and GL clients.
    file( WRITE "${output_file}"
        "#include \"FpsRenderer.h\"\n${source_prefix}${fps_game_draw}${source_suffix}" )

    message( STATUS "Generated FPS-only client interface: ${output_file}" )
endfunction()
