/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_shade.c

#include "tr_local.h" 

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/

/*
================
R_ArrayElementDiscrete

This is just for OpenGL conformance testing, it should never be the fastest
================
*/
static void APIENTRY R_ArrayElementDiscrete( GLint index ) {
	qglColor4ubv( tess.svars.colors[ index ] );
	if ( glState.currenttmu ) {
		qglMultiTexCoord2fARB( 0, tess.svars.texcoords[ 0 ][ index ][0], tess.svars.texcoords[ 0 ][ index ][1] );
		qglMultiTexCoord2fARB( 1, tess.svars.texcoords[ 1 ][ index ][0], tess.svars.texcoords[ 1 ][ index ][1] );
	} else {
		qglTexCoord2fv( tess.svars.texcoords[ 0 ][ index ] );
	}
	qglVertex3fv( tess.xyz[ index ] );
}

/*
===================
R_DrawStripElements

===================
*/
static int		c_vertexes;		// for seeing how long our average strips are
static int		c_begins;
static void R_DrawStripElements( int numIndexes, const glIndex_t *indexes, void ( APIENTRY *element )(GLint) ) {
	int i;
	glIndex_t last[3];
	qboolean even;

	c_begins++;

	if ( numIndexes <= 0 ) {
		return;
	}

	qglBegin( GL_TRIANGLE_STRIP );

	// prime the strip
	element( indexes[0] );
	element( indexes[1] );
	element( indexes[2] );
	c_vertexes += 3;

	last[0] = indexes[0];
	last[1] = indexes[1];
	last[2] = indexes[2];

	even = qfalse;

	for ( i = 3; i < numIndexes; i += 3 )
	{
		// odd numbered triangle in potential strip
		if ( !even )
		{
			// check previous triangle to see if we're continuing a strip
			if ( ( indexes[i+0] == last[2] ) && ( indexes[i+1] == last[1] ) )
			{
				element( indexes[i+2] );
				c_vertexes++;
				assert( indexes[i+2] < tess.numVertexes );
				even = qtrue;
			}
			// otherwise we're done with this strip so finish it and start
			// a new one
			else
			{
				qglEnd();

				qglBegin( GL_TRIANGLE_STRIP );
				c_begins++;

				element( indexes[i+0] );
				element( indexes[i+1] );
				element( indexes[i+2] );

				c_vertexes += 3;

				even = qfalse;
			}
		}
		else
		{
			// check previous triangle to see if we're continuing a strip
			if ( ( last[2] == indexes[i+1] ) && ( last[0] == indexes[i+0] ) )
			{
				element( indexes[i+2] );
				c_vertexes++;

				even = qfalse;
			}
			// otherwise we're done with this strip so finish it and start
			// a new one
			else
			{
				qglEnd();

				qglBegin( GL_TRIANGLE_STRIP );
				c_begins++;

				element( indexes[i+0] );
				element( indexes[i+1] );
				element( indexes[i+2] );
				c_vertexes += 3;

				even = qfalse;
			}
		}

		// cache the last three vertices
		last[0] = indexes[i+0];
		last[1] = indexes[i+1];
		last[2] = indexes[i+2];
	}

	qglEnd();
}



/*
==================
R_DrawElements

Optionally performs our own glDrawElements that looks for strip conditions
instead of using the single glDrawElements call that may be inefficient
without compiled vertex arrays.
==================
*/
void R_DrawElements( int numIndexes, const glIndex_t *indexes ) {
	int		primitives;

	primitives = r_primitives->integer;

	// default is to use triangles if compiled vertex arrays are present
	if ( primitives == 0 ) {
		if ( qglLockArraysEXT ) {
			primitives = 2;
		} else {
			primitives = 1;
		}
	}


	if ( primitives == 2 ) {
#ifdef __PS2__
		// the indexes are of tess's vertices: no index searched for (GL 1.2)
		glDrawRangeElements( GL_TRIANGLES, 0, tess.numVertexes - 1, numIndexes, GL_INDEX_TYPE, indexes );
#else
		qglDrawElements( GL_TRIANGLES, 
						numIndexes,
						GL_INDEX_TYPE,
						indexes );
#endif
		return;
	}

	if ( primitives == 1 ) {
		R_DrawStripElements( numIndexes,  indexes, qglArrayElement );
		return;
	}
	
	if ( primitives == 3 ) {
		R_DrawStripElements( numIndexes,  indexes, R_ArrayElementDiscrete );
		return;
	}

	// anything else will cause no drawing
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t	tess;
static qboolean	setArraysOnce;

/*
=================
R_BindAnimatedImage

=================
*/
static void R_BindAnimatedImage( textureBundle_t *bundle ) {
	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		GL_Bind( bundle->image[0] );
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	index = tess.shaderTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE;
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;	// may happen with shader time offsets
	}

	// Windows x86 doesn't load renderer DLL with 64 bit modulus
	//index %= bundle->numImageAnimations;
#ifdef __PS2__
	/* a 32-bit modulus: the loop below turns index / n times, thousands a
	   bind late in a long game (64-bit subtractions on the R5900) */
	if ( index <= 0x7FFFFFFF )
		index = (unsigned)index % (unsigned)bundle->numImageAnimations;
#endif
	while ( index >= bundle->numImageAnimations ) {
		index -= bundle->numImageAnimations;
	}

	GL_Bind( bundle->image[ index ] );
}

/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input) {
	GL_Bind( tr.whiteImage );
	qglColor3f (1,1,1);

	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );
	qglDepthRange( 0, 0 );

	qglDisableClientState (GL_COLOR_ARRAY);
	qglDisableClientState (GL_TEXTURE_COORD_ARRAY);

	qglVertexPointer (3, GL_FLOAT, 16, input->xyz);	// padded for SIMD

	if (qglLockArraysEXT) {
		qglLockArraysEXT(0, input->numVertexes);
		GLimp_LogComment( "glLockArraysEXT\n" );
	}

	R_DrawElements( input->numIndexes, input->indexes );

	if (qglUnlockArraysEXT) {
		qglUnlockArraysEXT();
		GLimp_LogComment( "glUnlockArraysEXT\n" );
	}
	qglDepthRange( 0, 1 );
}


/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	int		i;
	vec3_t	temp;

	GL_Bind( tr.whiteImage );
	qglColor3f (1,1,1);
	qglDepthRange( 0, 0 );	// never occluded
	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );

	qglBegin (GL_LINES);
	for (i = 0 ; i < input->numVertexes ; i++) {
		qglVertex3fv (input->xyz[i]);
		VectorMA (input->xyz[i], 2, input->normal[i], temp);
		qglVertex3fv (temp);
	}
	qglEnd ();

	qglDepthRange( 0, 1 );
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum ) {

	shader_t *state = (shader->remappedShader) ? shader->remappedShader : shader;

	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.shader = state;
	tess.fogNum = fogNum;
	tess.dlightBits = 0;		// will be OR'd in by surface functions
	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;
	tess.currentStageIteratorFunc = state->optimalStageIteratorFunc;

	tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
	if (tess.shader->clampTime && tess.shaderTime >= tess.shader->clampTime) {
		tess.shaderTime = tess.shader->clampTime;
	}


}

/*
===================
DrawMultitextured

output = t0 * t1 or t0 + t1

t0 = most upstream according to spec
t1 = most downstream according to spec
===================
*/
static void DrawMultitextured( shaderCommands_t *input, int stage ) {
	shaderStage_t	*pStage;

	pStage = tess.xstages[stage];

	GL_State( pStage->stateBits );

	// this is an ugly hack to work around a GeForce driver
	// bug with multitexture and clip planes
	if ( backEnd.viewParms.isPortal ) {
		qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
	}

	//
	// base
	//
	GL_SelectTexture( 0 );
	qglTexCoordPointer( 2, GL_FLOAT, 0, input->svars.texcoords[0] );
	R_BindAnimatedImage( &pStage->bundle[0] );

	//
	// lightmap/secondary pass
	//
	GL_SelectTexture( 1 );
	qglEnable( GL_TEXTURE_2D );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );

	if ( r_lightmap->integer ) {
		GL_TexEnv( GL_REPLACE );
	} else {
		GL_TexEnv( tess.shader->multitextureEnv );
	}

	qglTexCoordPointer( 2, GL_FLOAT, 0, input->svars.texcoords[1] );

	R_BindAnimatedImage( &pStage->bundle[1] );

	R_DrawElements( input->numIndexes, input->indexes );

	//
	// disable texturing on TEXTURE1, then select TEXTURE0
	//
	//qglDisableClientState( GL_TEXTURE_COORD_ARRAY );
	qglDisable( GL_TEXTURE_2D );

	GL_SelectTexture( 0 );
}



/*
===================
ProjectDlightTexture

Perform dynamic lighting with another rendering pass
===================
*/
static void ProjectDlightTexture_scalar( void ) {
	int		i, l;
	vec3_t	origin;
	float	*texCoords;
	byte	*colors;
	byte	clipBits[SHADER_MAX_VERTEXES];
	float	texCoordsArray[SHADER_MAX_VERTEXES][2];
	byte	colorArray[SHADER_MAX_VERTEXES][4];
	glIndex_t	hitIndexes[SHADER_MAX_INDEXES];
	int		numIndexes;
	float	scale;
	float	radius;
	vec3_t	floatColor;
	float	modulate = 0.0f;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}
		texCoords = texCoordsArray[0];
		colors = colorArray[0];

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		scale = 1.0f / radius;

		if(r_greyscale->integer)
		{
			float luminance;

			luminance = LUMA(dl->color[0], dl->color[1], dl->color[2]) * 255.0f;
			floatColor[0] = floatColor[1] = floatColor[2] = luminance;
		}
		else if(r_greyscale->value)
		{
			float luminance;
			
			luminance = LUMA(dl->color[0], dl->color[1], dl->color[2]) * 255.0f;
			floatColor[0] = LERP(dl->color[0] * 255.0f, luminance, r_greyscale->value);
			floatColor[1] = LERP(dl->color[1] * 255.0f, luminance, r_greyscale->value);
			floatColor[2] = LERP(dl->color[2] * 255.0f, luminance, r_greyscale->value);
		}
		else
		{
			floatColor[0] = dl->color[0] * 255.0f;
			floatColor[1] = dl->color[1] * 255.0f;
			floatColor[2] = dl->color[2] * 255.0f;
		}

		for ( i = 0 ; i < tess.numVertexes ; i++, texCoords += 2, colors += 4 ) {
			int		clip = 0;
			vec3_t	dist;
			
			VectorSubtract( origin, tess.xyz[i], dist );

			backEnd.pc.c_dlightVertexes++;

			texCoords[0] = 0.5f + dist[0] * scale;
			texCoords[1] = 0.5f + dist[1] * scale;

			if( !r_dlightBacks->integer &&
					// dist . tess.normal[i]
					( dist[0] * tess.normal[i][0] +
					dist[1] * tess.normal[i][1] +
					dist[2] * tess.normal[i][2] ) < 0.0f ) {
				clip = 63;
			} else {
				if ( texCoords[0] < 0.0f ) {
					clip |= 1;
				} else if ( texCoords[0] > 1.0f ) {
					clip |= 2;
				}
				if ( texCoords[1] < 0.0f ) {
					clip |= 4;
				} else if ( texCoords[1] > 1.0f ) {
					clip |= 8;
				}
				texCoords[0] = texCoords[0];
				texCoords[1] = texCoords[1];

				// modulate the strength based on the height and color
				if ( dist[2] > radius ) {
					clip |= 16;
					modulate = 0.0f;
				} else if ( dist[2] < -radius ) {
					clip |= 32;
					modulate = 0.0f;
				} else {
					dist[2] = Q_fabs(dist[2]);
					if ( dist[2] < radius * 0.5f ) {
						modulate = 1.0f;
					} else {
						modulate = 2.0f * (radius - dist[2]) * scale;
					}
				}
			}
			clipBits[i] = clip;
			colors[0] = ri.ftol(floatColor[0] * modulate);
			colors[1] = ri.ftol(floatColor[1] * modulate);
			colors[2] = ri.ftol(floatColor[2] * modulate);
#ifdef __PS2__
			// the GS brightens by the source alpha, not per channel
			// (libgl: DST_COLOR ONE is Cd * (1 + As)): the strongest channel
			colors[3] = MAX( colors[0], MAX( colors[1], colors[2] ) );
#else
			colors[3] = 255;
#endif
		}

		// build a list of triangles that need light
		numIndexes = 0;
		for ( i = 0 ; i < tess.numIndexes ; i += 3 ) {
			int		a, b, c;

			a = tess.indexes[i];
			b = tess.indexes[i+1];
			c = tess.indexes[i+2];
			if ( clipBits[a] & clipBits[b] & clipBits[c] ) {
				continue;	// not lighted
			}
			hitIndexes[numIndexes] = a;
			hitIndexes[numIndexes+1] = b;
			hitIndexes[numIndexes+2] = c;
			numIndexes += 3;
		}

		if ( !numIndexes ) {
			continue;
		}

		qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		qglTexCoordPointer( 2, GL_FLOAT, 0, texCoordsArray[0] );

		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, colorArray );

		GL_Bind( tr.dlightImage );
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		if ( dl->additive ) {
			GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		else {
			GL_State( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		}
		R_DrawElements( numIndexes, hitIndexes );
		backEnd.pc.c_totalIndexes += numIndexes;
		backEnd.pc.c_dlightIndexes += numIndexes;
	}
}

static void ProjectDlightTexture( void ) {
#if idppc_altivec
	if (com_altivec->integer) {
		// must be in a separate translation unit or G3 systems will crash.
		ProjectDlightTexture_altivec();
		return;
	}
#endif
	ProjectDlightTexture_scalar();
}


/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	fog_t		*fog;
	int			i;

	qglEnableClientState( GL_COLOR_ARRAY );
	qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, tess.svars.colors );

	qglEnableClientState( GL_TEXTURE_COORD_ARRAY);
	qglTexCoordPointer( 2, GL_FLOAT, 0, tess.svars.texcoords[0] );

	fog = tr.world->fogs + tess.fogNum;

	for ( i = 0; i < tess.numVertexes; i++ ) {
		* ( int * )&tess.svars.colors[i] = fog->colorInt;
	}

	RB_CalcFogTexCoords( ( float * ) tess.svars.texcoords[0] );

	GL_Bind( tr.fogImage );

	if ( tess.shader->fogPass == FP_EQUAL ) {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}

	R_DrawElements( tess.numIndexes, tess.indexes );
}

/*
===============
ComputeColors
===============
*/
static void ComputeColors( shaderStage_t *pStage )
{
	int		i;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
		case CGEN_IDENTITY:
			Com_Memset( tess.svars.colors, 0xff, tess.numVertexes * 4 );
			break;
		default:
		case CGEN_IDENTITY_LIGHTING:
			Com_Memset( tess.svars.colors, tr.identityLightByte, tess.numVertexes * 4 );
			break;
		case CGEN_LIGHTING_DIFFUSE:
			RB_CalcDiffuseColor( ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_EXACT_VERTEX:
			Com_Memcpy( tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof( tess.vertexColors[0] ) );
			break;
		case CGEN_CONST:
			for ( i = 0; i < tess.numVertexes; i++ ) {
				*(int *)tess.svars.colors[i] = *(int *)pStage->constantColor;
			}
			break;
		case CGEN_VERTEX:
			if ( tr.identityLight == 1 )
			{
				Com_Memcpy( tess.svars.colors, tess.vertexColors, tess.numVertexes * sizeof( tess.vertexColors[0] ) );
			}
			else
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = tess.vertexColors[i][0] * tr.identityLight;
					tess.svars.colors[i][1] = tess.vertexColors[i][1] * tr.identityLight;
					tess.svars.colors[i][2] = tess.vertexColors[i][2] * tr.identityLight;
					tess.svars.colors[i][3] = tess.vertexColors[i][3];
				}
			}
			break;
		case CGEN_ONE_MINUS_VERTEX:
			if ( tr.identityLight == 1 )
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = 255 - tess.vertexColors[i][0];
					tess.svars.colors[i][1] = 255 - tess.vertexColors[i][1];
					tess.svars.colors[i][2] = 255 - tess.vertexColors[i][2];
				}
			}
			else
			{
				for ( i = 0; i < tess.numVertexes; i++ )
				{
					tess.svars.colors[i][0] = ( 255 - tess.vertexColors[i][0] ) * tr.identityLight;
					tess.svars.colors[i][1] = ( 255 - tess.vertexColors[i][1] ) * tr.identityLight;
					tess.svars.colors[i][2] = ( 255 - tess.vertexColors[i][2] ) * tr.identityLight;
				}
			}
			break;
		case CGEN_FOG:
			{
				fog_t		*fog;

				fog = tr.world->fogs + tess.fogNum;

				for ( i = 0; i < tess.numVertexes; i++ ) {
					* ( int * )&tess.svars.colors[i] = fog->colorInt;
				}
			}
			break;
		case CGEN_WAVEFORM:
			RB_CalcWaveColor( &pStage->rgbWave, ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_ENTITY:
			RB_CalcColorFromEntity( ( unsigned char * ) tess.svars.colors );
			break;
		case CGEN_ONE_MINUS_ENTITY:
			RB_CalcColorFromOneMinusEntity( ( unsigned char * ) tess.svars.colors );
			break;
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
	case AGEN_SKIP:
		break;
	case AGEN_IDENTITY:
#ifdef __PS2__
		/* RB_CalcDiffuseColor and RB_CalcWaveColor give alpha 255 already */
		if ( pStage->rgbGen == CGEN_LIGHTING_DIFFUSE || pStage->rgbGen == CGEN_WAVEFORM )
			break;
#endif
		if ( pStage->rgbGen != CGEN_IDENTITY ) {
			if ( ( pStage->rgbGen == CGEN_VERTEX && tr.identityLight != 1 ) ||
				 pStage->rgbGen != CGEN_VERTEX ) {
				for ( i = 0; i < tess.numVertexes; i++ ) {
					tess.svars.colors[i][3] = 0xff;
				}
			}
		}
		break;
	case AGEN_CONST:
		if ( pStage->rgbGen != CGEN_CONST ) {
			for ( i = 0; i < tess.numVertexes; i++ ) {
				tess.svars.colors[i][3] = pStage->constantColor[3];
			}
		}
		break;
	case AGEN_WAVEFORM:
		RB_CalcWaveAlpha( &pStage->alphaWave, ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_LIGHTING_SPECULAR:
		RB_CalcSpecularAlpha( ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_ENTITY:
		RB_CalcAlphaFromEntity( ( unsigned char * ) tess.svars.colors );
		break;
	case AGEN_ONE_MINUS_ENTITY:
		RB_CalcAlphaFromOneMinusEntity( ( unsigned char * ) tess.svars.colors );
		break;
    case AGEN_VERTEX:
		if ( pStage->rgbGen != CGEN_VERTEX ) {
			for ( i = 0; i < tess.numVertexes; i++ ) {
				tess.svars.colors[i][3] = tess.vertexColors[i][3];
			}
		}
        break;
    case AGEN_ONE_MINUS_VERTEX:
        for ( i = 0; i < tess.numVertexes; i++ )
        {
			tess.svars.colors[i][3] = 255 - tess.vertexColors[i][3];
        }
        break;
	case AGEN_PORTAL:
		{
			unsigned char alpha;

			for ( i = 0; i < tess.numVertexes; i++ )
			{
				float len;
				vec3_t v;

				VectorSubtract( tess.xyz[i], backEnd.viewParms.or.origin, v );
				len = VectorLength( v );

				len /= tess.shader->portalRange;

				if ( len < 0 )
				{
					alpha = 0;
				}
				else if ( len > 1 )
				{
					alpha = 0xff;
				}
				else
				{
					alpha = len * 0xff;
				}

				tess.svars.colors[i][3] = alpha;
			}
		}
		break;
	}

	//
	// fog adjustment for colors to fade out as fog increases
	//
	if ( tess.fogNum )
	{
		switch ( pStage->adjustColorsForFog )
		{
		case ACFF_MODULATE_RGB:
			RB_CalcModulateColorsByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_MODULATE_ALPHA:
			RB_CalcModulateAlphasByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_MODULATE_RGBA:
			RB_CalcModulateRGBAsByFog( ( unsigned char * ) tess.svars.colors );
			break;
		case ACFF_NONE:
			break;
		}
	}
	
	// if in greyscale rendering mode turn all color values into greyscale.
	if(r_greyscale->integer)
	{
		int scale;
		for(i = 0; i < tess.numVertexes; i++)
		{
			scale = LUMA(tess.svars.colors[i][0], tess.svars.colors[i][1], tess.svars.colors[i][2]);
 			tess.svars.colors[i][0] = tess.svars.colors[i][1] = tess.svars.colors[i][2] = scale;
		}
	}
	else if(r_greyscale->value)
	{
		float scale;
		
		for(i = 0; i < tess.numVertexes; i++)
		{
			scale = LUMA(tess.svars.colors[i][0], tess.svars.colors[i][1], tess.svars.colors[i][2]);
			tess.svars.colors[i][0] = LERP(tess.svars.colors[i][0], scale, r_greyscale->value);
			tess.svars.colors[i][1] = LERP(tess.svars.colors[i][1], scale, r_greyscale->value);
			tess.svars.colors[i][2] = LERP(tess.svars.colors[i][2], scale, r_greyscale->value);
		}
	}
}

/*
===============
ComputeTexCoords
===============
*/
#ifdef __PS2__
/*
The tcMods of a stage that are affine (all but turb) as GL's texture
matrix, not done to each vertex on the EE: m is s' = m[0] s + m[2] t + m[4],
t' = m[1] s + m[3] t + m[5]. NULL: the identity.
*/
static qboolean ps2_texMatrixOn;
float RB_PS2_EvalWaveForm( const waveForm_t *wf );
void RB_PS2_TexMatrix( const float *m ) {
	float	g[16];

	if ( !m && !ps2_texMatrixOn ) {
		return;
	}
	Com_Memset( g, 0, sizeof( g ) );
	g[10] = g[15] = 1;
	if ( m ) {
		g[0] = m[0], g[1] = m[1], g[4] = m[2], g[5] = m[3], g[12] = m[4], g[13] = m[5];
	} else {
		g[0] = g[5] = 1;
	}
	qglMatrixMode( GL_TEXTURE );
	qglLoadMatrixf( g );
	qglMatrixMode( GL_MODELVIEW );
	ps2_texMatrixOn = m != NULL;
}

/* r = a then b (b applied to a's result) */
static void PS2_TexMatMul( float *r, const float *b ) {
	float a[6];
	Com_Memcpy( a, r, sizeof( a ) );
	r[0] = b[0] * a[0] + b[2] * a[1];
	r[1] = b[1] * a[0] + b[3] * a[1];
	r[2] = b[0] * a[2] + b[2] * a[3];
	r[3] = b[1] * a[2] + b[3] * a[3];
	r[4] = b[0] * a[4] + b[2] * a[5] + b[4];
	r[5] = b[1] * a[4] + b[3] * a[5] + b[5];
}

/* the stage's tcMods of bundle b as a matrix in m; qfalse if one is not affine */
static qboolean PS2_TexModMatrix( shaderStage_t *pStage, int b, float *m ) {
	int tm;

	m[0] = m[3] = 1, m[1] = m[2] = m[4] = m[5] = 0;
	for ( tm = 0; tm < pStage->bundle[b].numTexMods; tm++ ) {
		texModInfo_t *t = &pStage->bundle[b].texMods[tm];
		float a[6] = { 1, 0, 0, 1, 0, 0 };
		double sc[2];

		switch ( t->type ) {
		case TMOD_NONE:
			return qtrue;
		case TMOD_TURBULENT:
			return qfalse;
		case TMOD_ENTITY_TRANSLATE:
		case TMOD_SCROLL: {
			const float *sp = t->type == TMOD_SCROLL ? t->scroll : backEnd.currentEntity->e.shaderTexCoord;
			sc[0] = sp[0] * tess.shaderTime, sc[1] = sp[1] * tess.shaderTime;
			a[4] = sc[0] - floor( sc[0] ), a[5] = sc[1] - floor( sc[1] );
			break;
		}
		case TMOD_SCALE:
			a[0] = t->scale[0], a[3] = t->scale[1];
			break;
		case TMOD_STRETCH: {
			float p = 1.0f / RB_PS2_EvalWaveForm( &t->wave );
			a[0] = a[3] = p, a[4] = a[5] = 0.5f - 0.5f * p;
			break;
		}
		case TMOD_TRANSFORM:
			a[0] = t->matrix[0][0], a[1] = t->matrix[0][1], a[2] = t->matrix[1][0], a[3] = t->matrix[1][1];
			a[4] = t->translate[0], a[5] = t->translate[1];
			break;
		case TMOD_ROTATE: {
			double degs = -t->rotateSpeed * tess.shaderTime;
			int64_t index = degs * ( FUNCTABLE_SIZE / 360.0f );
			float sinValue = tr.sinTable[ index & FUNCTABLE_MASK ];
			float cosValue = tr.sinTable[ ( index + FUNCTABLE_SIZE / 4 ) & FUNCTABLE_MASK ];
			a[0] = cosValue, a[2] = -sinValue, a[4] = 0.5 - 0.5 * cosValue + 0.5 * sinValue;
			a[1] = sinValue, a[3] = cosValue, a[5] = 0.5 - 0.5 * sinValue - 0.5 * cosValue;
			break;
		}
		default:
			return qfalse;
		}
		PS2_TexMatMul( m, a );
	}
	return qtrue;
}
#endif

static void ComputeTexCoords( shaderStage_t *pStage ) {
	int		i;
	int		b;
#ifdef __PS2__
	float	texMatrix[6];
	qboolean texMatrixUsed = qfalse;
#endif

	for ( b = 0; b < NUM_TEXTURE_BUNDLES; b++ ) {
		int tm;

		//
		// generate the texture coordinates
		//
		switch ( pStage->bundle[b].tcGen )
		{
		case TCGEN_IDENTITY:
			Com_Memset( tess.svars.texcoords[b], 0, sizeof( float ) * 2 * tess.numVertexes );
			break;
		case TCGEN_TEXTURE:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][0][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][0][1];
			}
			break;
		case TCGEN_LIGHTMAP:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = tess.texCoords[i][1][0];
				tess.svars.texcoords[b][i][1] = tess.texCoords[i][1][1];
			}
			break;
		case TCGEN_VECTOR:
			for ( i = 0 ; i < tess.numVertexes ; i++ ) {
				tess.svars.texcoords[b][i][0] = DotProduct( tess.xyz[i], pStage->bundle[b].tcGenVectors[0] );
				tess.svars.texcoords[b][i][1] = DotProduct( tess.xyz[i], pStage->bundle[b].tcGenVectors[1] );
			}
			break;
		case TCGEN_FOG:
			RB_CalcFogTexCoords( ( float * ) tess.svars.texcoords[b] );
			break;
		case TCGEN_ENVIRONMENT_MAPPED:
			RB_CalcEnvironmentTexCoords( ( float * ) tess.svars.texcoords[b] );
			break;
		case TCGEN_BAD:
#ifdef __PS2__
			goto ps2_done;
#else
			return;
#endif
		}

		//
		// alter texture coordinates
		//
#ifdef __PS2__
		// unit 0's affine ones: the texture matrix
		if ( b == 0 && pStage->bundle[0].numTexMods && PS2_TexModMatrix( pStage, 0, texMatrix ) ) {
			texMatrixUsed = qtrue;
			continue;
		}
#endif
		for ( tm = 0; tm < pStage->bundle[b].numTexMods ; tm++ ) {
			switch ( pStage->bundle[b].texMods[tm].type )
			{
			case TMOD_NONE:
				tm = TR_MAX_TEXMODS;		// break out of for loop
				break;

			case TMOD_TURBULENT:
				RB_CalcTurbulentTexCoords( &pStage->bundle[b].texMods[tm].wave, 
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ENTITY_TRANSLATE:
				RB_CalcScrollTexCoords( backEnd.currentEntity->e.shaderTexCoord,
									 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCROLL:
				RB_CalcScrollTexCoords( pStage->bundle[b].texMods[tm].scroll,
										 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_SCALE:
				RB_CalcScaleTexCoords( pStage->bundle[b].texMods[tm].scale,
									 ( float * ) tess.svars.texcoords[b] );
				break;
			
			case TMOD_STRETCH:
				RB_CalcStretchTexCoords( &pStage->bundle[b].texMods[tm].wave, 
						               ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_TRANSFORM:
				RB_CalcTransformTexCoords( &pStage->bundle[b].texMods[tm],
						                 ( float * ) tess.svars.texcoords[b] );
				break;

			case TMOD_ROTATE:
				RB_CalcRotateTexCoords( pStage->bundle[b].texMods[tm].rotateSpeed,
										( float * ) tess.svars.texcoords[b] );
				break;

			default:
				ri.Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", pStage->bundle[b].texMods[tm].type, tess.shader->name );
				break;
			}
		}
	}
#ifdef __PS2__
ps2_done:
	RB_PS2_TexMatrix( texMatrixUsed ? texMatrix : NULL );
#endif
}

/*
** RB_IterateStagesGeneric
*/
#ifdef __PS2__
/*
The GS cannot multiply by the destination colour, only by the source
alpha: a blendFunc filter stage is drawn as Cd * As with a lightmap kept as
its brightness (lightmaps are GL_LUMINANCE8), up to 2x. A lightmap stage followed
by a filter stage is drawn the other way round: the texture first, with the
lightmap stage's state, then the lightmap as the filter. The stages under a
lightmap filter are drawn without the overbright shift (the lightmap's 2x
brings it), the others with it. The lightmap gives only the brightness:
the stage under it, if white (rgbGen identity), takes the hue of the map's
vertex light (the colour scaled to a largest channel of 255).
*/
#define PS2_BLEND_BITS ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS )
static qboolean RB_PS2_IsFilter( unsigned bits ) {
	bits &= PS2_BLEND_BITS;
	// no destination alpha (libgl): ONE_MINUS_DST_ALPHA is ZERO
	return bits == ( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO ) ||
	       bits == ( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE_MINUS_DST_ALPHA ) ||
	       bits == ( GLS_SRCBLEND_ZERO | GLS_DSTBLEND_SRC_COLOR );
}

/*
The stage's colours as vertex colour * mul + add (q3stage.hlsl on VU1:
no loop over the vertices here), as ComputeColors makes them: rgbGen and
alphaGen that are a constant or affine in the vertex colour; a constant
from tr_shade_calc.c's own functions made for one vertex. hue: rgb
identity becomes the vertex colour's hue (add -255). qfalse: not so (the
lighting ones, fog, a list being made: its colours are recorded).
*/
static qboolean PS2_StageColors( shaderStage_t *pStage, qboolean hue, float *mul, float *add ) {
	byte c[4] = { 255, 255, 255, 255 };
	float il = tr.identityLight;
	int n, i, alpha = 0;	// alpha: 1 when rgbGen set it

	if ( ps2_listCompiling || r_greyscale->value || ( tess.fogNum && pStage->adjustColorsForFog != ACFF_NONE ) ) {
		return qfalse;
	}
	n = tess.numVertexes;
	tess.numVertexes = 1;
	for ( i = 0; i < 4; i++ ) {
		mul[i] = 0, add[i] = 0;
	}
	switch ( pStage->rgbGen ) {
	case CGEN_IDENTITY:
		add[0] = add[1] = add[2] = add[3] = 255;
		alpha = 1;
		break;
	case CGEN_IDENTITY_LIGHTING:
		add[0] = add[1] = add[2] = add[3] = tr.identityLightByte;
		alpha = 1;
		break;
	case CGEN_EXACT_VERTEX:
		mul[0] = mul[1] = mul[2] = mul[3] = 1;
		alpha = 1;
		break;
	case CGEN_VERTEX:
		mul[0] = mul[1] = mul[2] = il, mul[3] = 1;
		alpha = 1;
		break;
	case CGEN_ONE_MINUS_VERTEX:
		mul[0] = mul[1] = mul[2] = -il;
		add[0] = add[1] = add[2] = 255 * il;
		break;
	case CGEN_CONST:
		Com_Memcpy( c, pStage->constantColor, 4 );
		alpha = 2;
		break;
	case CGEN_FOG:
		Com_Memcpy( c, &tr.world->fogs[tess.fogNum].colorInt, 4 );
		alpha = 2;
		break;
	case CGEN_WAVEFORM:
		RB_CalcWaveColor( &pStage->rgbWave, c );
		alpha = 2;
		break;
	case CGEN_ENTITY:
		RB_CalcColorFromEntity( c );
		alpha = 2;
		break;
	case CGEN_ONE_MINUS_ENTITY:
		RB_CalcColorFromOneMinusEntity( c );
		alpha = 2;
		break;
	default:
		tess.numVertexes = n;
		return qfalse;
	}
	if ( alpha == 2 ) {
		for ( i = 0; i < 4; i++ ) {
			add[i] = c[i];
		}
	}
	if ( hue && ( pStage->rgbGen == CGEN_IDENTITY || pStage->rgbGen == CGEN_IDENTITY_LIGHTING ) ) {
		mul[0] = mul[1] = mul[2] = 0;
		add[0] = add[1] = add[2] = -255;
	}
	switch ( pStage->alphaGen ) {
	case AGEN_SKIP:
		break;
	case AGEN_IDENTITY:
		if ( pStage->rgbGen != CGEN_IDENTITY && !( pStage->rgbGen == CGEN_VERTEX && il == 1 ) ) {
			mul[3] = 0, add[3] = 255;
			alpha = 1;
		}
		break;
	case AGEN_CONST:
		if ( pStage->rgbGen != CGEN_CONST ) {
			mul[3] = 0, add[3] = pStage->constantColor[3];
			alpha = 1;
		}
		break;
	case AGEN_WAVEFORM:
		RB_CalcWaveAlpha( &pStage->alphaWave, c );
		mul[3] = 0, add[3] = c[3];
		alpha = 1;
		break;
	case AGEN_ENTITY:
		RB_CalcAlphaFromEntity( c );
		mul[3] = 0, add[3] = c[3];
		alpha = 1;
		break;
	case AGEN_ONE_MINUS_ENTITY:
		RB_CalcAlphaFromOneMinusEntity( c );
		mul[3] = 0, add[3] = c[3];
		alpha = 1;
		break;
	case AGEN_VERTEX:
		if ( pStage->rgbGen != CGEN_VERTEX ) {
			mul[3] = 1, add[3] = 0;
			alpha = 1;
		}
		break;
	case AGEN_ONE_MINUS_VERTEX:
		mul[3] = -1, add[3] = 255;
		alpha = 1;
		break;
	default:
		alpha = 0;
		break;
	}
	tess.numVertexes = n;
	// no alpha from either (oneMinusVertex without an alphaGen: the last
	// stage's alpha was kept), or one the program cannot make
	return alpha != 0 && ( pStage->alphaGen != AGEN_LIGHTING_SPECULAR && pStage->alphaGen != AGEN_PORTAL );
}

/* stage's texture coordinates as an array (tess.texCoords, its own
   coordinates or the lightmap's) with its tcMods in the texture matrix,
   or NULL (ComputeTexCoords makes them) */
static const float *PS2_StageTexCoords( shaderStage_t *pStage ) {
	float m[6];
	int b;

	switch ( pStage->bundle[0].tcGen ) {
	case TCGEN_TEXTURE: b = 0; break;
	case TCGEN_LIGHTMAP: b = 1; break;
	default: return NULL;
	}
	if ( pStage->bundle[0].numTexMods && !PS2_TexModMatrix( pStage, 0, m ) ) {
		return NULL;
	}
	RB_PS2_TexMatrix( pStage->bundle[0].numTexMods ? m : NULL );
	return tess.texCoords[0][b];
}

static void RB_PS2_DrawStage( shaderCommands_t *input, shaderStage_t *pStage, unsigned bits, int shift, qboolean hue ) {
	float mul[4], add[4];
	const float *tc;

	/* on VU1 (q3stage.hlsl): the vertex colours and coordinates as they are */
	if ( PS2_StageColors( pStage, hue, mul, add ) && ( tc = PS2_StageTexCoords( pStage ) ) != NULL &&
	     GL_PS2_StageProgram( mul, add ) ) {
		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, input->vertexColors );
		qglTexCoordPointer( 2, GL_FLOAT, sizeof( input->texCoords[0] ), tc );
		R_BindAnimatedImage( &pStage->bundle[0] );
		GL_State( bits );
		GL_PS2_Shift( shift );
		R_DrawElements( input->numIndexes, input->indexes );
		GL_PS2_Shift( tr.overbrightBits );
		GL_PS2_StageProgram( NULL, NULL );
		return;
	}
	ComputeColors( pStage );
	ComputeTexCoords( pStage );
	if ( hue && ( pStage->rgbGen == CGEN_IDENTITY || pStage->rgbGen == CGEN_IDENTITY_LIGHTING ) ) {
		int i;
		for ( i = 0; i < input->numVertexes; i++ ) {
			const byte *v = input->vertexColors[i];
			byte *c = input->svars.colors[i];
			int m = v[0] > v[1] ? v[0] : v[1];
			m = m > v[2] ? m : v[2];
			if ( m < 16 ) {
				c[0] = c[1] = c[2] = 255;
			} else {
				c[0] = v[0] * 255 / m;
				c[1] = v[1] * 255 / m;
				c[2] = v[2] * 255 / m;
			}
		}
	}
	qglEnableClientState( GL_COLOR_ARRAY );
	qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, input->svars.colors );
	qglTexCoordPointer( 2, GL_FLOAT, 0, input->svars.texcoords[0] );
	R_BindAnimatedImage( &pStage->bundle[0] );
	GL_State( bits );
	GL_PS2_Shift( shift );
	R_DrawElements( input->numIndexes, input->indexes );
	GL_PS2_Shift( tr.overbrightBits );
}

/* qtrue if stage (and maybe the next) was drawn */
static qboolean RB_PS2_Stages( shaderCommands_t *input, int *stage ) {
	shaderStage_t *s = tess.xstages[*stage];
	shaderStage_t *n = *stage + 1 < MAX_SHADER_STAGES ? tess.xstages[*stage + 1] : NULL;

	if ( r_lightmap->integer || s->bundle[1].image[0] || !n || n->bundle[1].image[0] ||
	     !RB_PS2_IsFilter( n->stateBits ) )
		return qfalse;
	if ( s->bundle[0].isLightmap && !( s->stateBits & PS2_BLEND_BITS ) && !n->bundle[0].isLightmap ) {
		RB_PS2_DrawStage( input, n, s->stateBits | ( n->stateBits & GLS_ATEST_BITS ), 0, !ps2_colorlight );
		RB_PS2_DrawStage( input, s, n->stateBits & ~GLS_ATEST_BITS, tr.overbrightBits, qfalse );
		( *stage )++;
		return qtrue;
	}
	if ( n->bundle[0].isLightmap && ( s->stateBits & GLS_ATEST_BITS ) ) {
		/* the lightmap's depthFunc equal would be GEQUAL (the GS has no
		   EQUAL): it would also land on what is seen through the holes the
		   alpha test made. Drawn once, lit by the vertex light instead */
		ComputeColors( s );
		if ( s->rgbGen == CGEN_IDENTITY || s->rgbGen == CGEN_IDENTITY_LIGHTING )
			Com_Memcpy( input->svars.colors, input->vertexColors, input->numVertexes * 4 );
		ComputeTexCoords( s );
		qglEnableClientState( GL_COLOR_ARRAY );
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, input->svars.colors );
		qglTexCoordPointer( 2, GL_FLOAT, 0, input->svars.texcoords[0] );
		R_BindAnimatedImage( &s->bundle[0] );
		GL_State( s->stateBits );
		R_DrawElements( input->numIndexes, input->indexes );
		( *stage )++;
		return qtrue;
	}
	if ( n->bundle[0].isLightmap ) {
		RB_PS2_DrawStage( input, s, s->stateBits, 0, !ps2_colorlight );
		return qtrue;
	}
	return qfalse;
}
#endif

static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;
#ifdef __PS2__
	// the stages under a lightmap filter: without the overbright shift
	int lmf = 0;
	for ( stage = 0; stage < MAX_SHADER_STAGES && tess.xstages[stage]; stage++ )
		if ( tess.xstages[stage]->bundle[0].isLightmap && RB_PS2_IsFilter( tess.xstages[stage]->stateBits ) )
			lmf = stage;
#endif

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = tess.xstages[stage];

		if ( !pStage )
		{
			break;
		}

#ifdef __PS2__
		if ( RB_PS2_Stages( input, &stage ) )
			continue;
		/* one texture: its colours on VU1 when they can be (RB_PS2_DrawStage) */
		if ( !pStage->bundle[1].image[0] ) {
			RB_PS2_DrawStage( input, pStage, pStage->stateBits, stage < lmf ? 0 : tr.overbrightBits, qfalse );
			if ( r_lightmap->integer && pStage->bundle[0].isLightmap )
				break;
			continue;
		}
#endif

		ComputeColors( pStage );
		ComputeTexCoords( pStage );

		if ( !setArraysOnce )
		{
			qglEnableClientState( GL_COLOR_ARRAY );
			qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, input->svars.colors );
		}

		//
		// do multitexture
		//
		if ( pStage->bundle[1].image[0] != 0 )
		{
			DrawMultitextured( input, stage );
		}
		else
		{
			if ( !setArraysOnce )
			{
				qglTexCoordPointer( 2, GL_FLOAT, 0, input->svars.texcoords[0] );
			}

			//
			// set state
			//
			R_BindAnimatedImage( &pStage->bundle[0] );

			GL_State( pStage->stateBits );

			//
			// draw
			//
#ifdef __PS2__
			GL_PS2_Shift( stage < lmf ? 0 : tr.overbrightBits );
#endif
			R_DrawElements( input->numIndexes, input->indexes );
#ifdef __PS2__
			GL_PS2_Shift( tr.overbrightBits );
#endif
		}
		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) )
		{
			break;
		}
	}
}


/*
** RB_StageIteratorGeneric
*/
void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;
	shader_t		*shader;

	input = &tess;
	shader = input->shader;

	RB_DeformTessGeometry();

	//
	// log this call
	//
	if ( r_logFile->integer ) 
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	GL_Cull( shader->cullType );

	// set polygon offset if necessary
	if ( shader->polygonOffset )
	{
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	}

	//
	// if there is only a single pass then we can enable color
	// and texture arrays before we compile, otherwise we need
	// to avoid compiling those arrays since they will change
	// during multipass rendering
	//
	if ( tess.numPasses > 1 || shader->multitextureEnv )
	{
		setArraysOnce = qfalse;
		qglDisableClientState (GL_COLOR_ARRAY);
		qglDisableClientState (GL_TEXTURE_COORD_ARRAY);
	}
	else
	{
		setArraysOnce = qtrue;

		qglEnableClientState( GL_COLOR_ARRAY);
		qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, tess.svars.colors );

		qglEnableClientState( GL_TEXTURE_COORD_ARRAY);
		qglTexCoordPointer( 2, GL_FLOAT, 0, tess.svars.texcoords[0] );
	}

	//
	// lock XYZ
	//
	qglVertexPointer (3, GL_FLOAT, 16, input->xyz);	// padded for SIMD
	if (qglLockArraysEXT)
	{
		qglLockArraysEXT(0, input->numVertexes);
		GLimp_LogComment( "glLockArraysEXT\n" );
	}

	//
	// enable color and texcoord arrays after the lock if necessary
	//
	if ( !setArraysOnce )
	{
		qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
		qglEnableClientState( GL_COLOR_ARRAY );
	}

	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );
#ifdef __PS2__
	RB_PS2_TexMatrix( NULL );	// the dlight and fog passes, and draws after: none
#endif

	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE
		&& !(tess.shader->surfaceFlags & (SURF_NODLIGHT | SURF_SKY) ) ) {
		ProjectDlightTexture();
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	// 
	// unlock arrays
	//
	if (qglUnlockArraysEXT) 
	{
		qglUnlockArraysEXT();
		GLimp_LogComment( "glUnlockArraysEXT\n" );
	}

	//
	// reset polygon offset
	//
	if ( shader->polygonOffset )
	{
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
}


/*
** RB_StageIteratorVertexLitTexture
*/
void RB_StageIteratorVertexLitTexture( void )
{
	shaderCommands_t *input;
	shader_t		*shader;

	input = &tess;
	shader = input->shader;

	//
	// compute colors
	//
	RB_CalcDiffuseColor( ( unsigned char * ) tess.svars.colors );

	//
	// log this call
	//
	if ( r_logFile->integer ) 
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorVertexLitTexturedUnfogged( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	GL_Cull( shader->cullType );

	//
	// set arrays and lock
	//
	qglEnableClientState( GL_COLOR_ARRAY);
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY);

	qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, tess.svars.colors );
	qglTexCoordPointer( 2, GL_FLOAT, 16, tess.texCoords[0][0] );
	qglVertexPointer (3, GL_FLOAT, 16, input->xyz);

	if ( qglLockArraysEXT )
	{
		qglLockArraysEXT(0, input->numVertexes);
		GLimp_LogComment( "glLockArraysEXT\n" );
	}

	//
	// call special shade routine
	//
	R_BindAnimatedImage( &tess.xstages[0]->bundle[0] );
	GL_State( tess.xstages[0]->stateBits );
	R_DrawElements( input->numIndexes, input->indexes );

	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE ) {
		ProjectDlightTexture();
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	// 
	// unlock arrays
	//
	if (qglUnlockArraysEXT) 
	{
		qglUnlockArraysEXT();
		GLimp_LogComment( "glUnlockArraysEXT\n" );
	}
}

//define	REPLACE_MODE

void RB_StageIteratorLightmappedMultitexture( void ) {
	shaderCommands_t *input;
	shader_t		*shader;

	input = &tess;
	shader = input->shader;

	//
	// log this call
	//
	if ( r_logFile->integer ) {
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorLightmappedMultitexture( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	GL_Cull( shader->cullType );

	//
	// set color, pointers, and lock
	//
	GL_State( GLS_DEFAULT );
	qglVertexPointer( 3, GL_FLOAT, 16, input->xyz );

#ifdef REPLACE_MODE
	qglDisableClientState( GL_COLOR_ARRAY );
	qglColor3f( 1, 1, 1 );
	qglShadeModel( GL_FLAT );
#else
	qglEnableClientState( GL_COLOR_ARRAY );
	qglColorPointer( 4, GL_UNSIGNED_BYTE, 0, tess.constantColor255 );
#endif

	//
	// select base stage
	//
	GL_SelectTexture( 0 );

	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	R_BindAnimatedImage( &tess.xstages[0]->bundle[0] );
	qglTexCoordPointer( 2, GL_FLOAT, 16, tess.texCoords[0][0] );

	//
	// configure second stage
	//
	GL_SelectTexture( 1 );
	qglEnable( GL_TEXTURE_2D );
	if ( r_lightmap->integer ) {
		GL_TexEnv( GL_REPLACE );
	} else {
		GL_TexEnv( GL_MODULATE );
	}
	R_BindAnimatedImage( &tess.xstages[0]->bundle[1] );
	qglEnableClientState( GL_TEXTURE_COORD_ARRAY );
	qglTexCoordPointer( 2, GL_FLOAT, 16, tess.texCoords[0][1] );

	//
	// lock arrays
	//
	if ( qglLockArraysEXT ) {
		qglLockArraysEXT(0, input->numVertexes);
		GLimp_LogComment( "glLockArraysEXT\n" );
	}

	R_DrawElements( input->numIndexes, input->indexes );

	//
	// disable texturing on TEXTURE1, then select TEXTURE0
	//
	qglDisable( GL_TEXTURE_2D );
	qglDisableClientState( GL_TEXTURE_COORD_ARRAY );

	GL_SelectTexture( 0 );
#ifdef REPLACE_MODE
	GL_TexEnv( GL_MODULATE );
	qglShadeModel( GL_SMOOTH );
#endif

	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE ) {
		ProjectDlightTexture();
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	//
	// unlock arrays
	//
	if ( qglUnlockArraysEXT ) {
		qglUnlockArraysEXT();
		GLimp_LogComment( "glUnlockArraysEXT\n" );
	}
}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0) {
		return;
	}
#ifdef Q3_PERF
	extern unsigned long long ps2_perf[10];
	extern int ps2_perf_mesh;
	unsigned pc0;
	__asm__ volatile( "mfc0 %0, $9" : "=r"( pc0 ) );
	int pmesh = ps2_perf_mesh, pverts = tess.numVertexes, pidx = tess.numIndexes;
	ps2_perf_mesh = 0;
#endif

	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}	
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer ) {
		DrawTris (input);
	}
	if ( r_shownormals->integer ) {
		DrawNormals (input);
	}
	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
#ifdef Q3_PERF
	{
		unsigned pc1;
		__asm__ volatile( "mfc0 %0, $9" : "=r"( pc1 ) );
		if ( pmesh ) {
			ps2_perf[4]++, ps2_perf[5] += pc1 - pc0;
		}
		ps2_perf[6]++, ps2_perf[7] += pc1 - pc0;
		ps2_perf[8] += pverts, ps2_perf[9] += (unsigned long long)pidx * tess.numPasses;
	}
#endif

	GLimp_LogComment( "----------\n" );
}

