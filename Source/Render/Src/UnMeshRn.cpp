/*=============================================================================
	UnMeshRn.cpp: Unreal mesh rendering.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "RenderPrivate.h"

/*------------------------------------------------------------------------------
	Globals.
------------------------------------------------------------------------------*/

UBOOL               HasSpecialCoords;
FCoords             SpecialCoords;
static FLOAT        UScale, VScale;
static UTexture*    Textures[16];
static FTextureInfo TextureInfo[16];
static FTextureInfo EnvironmentInfo;
static FVector      GUnlitColor;

/*------------------------------------------------------------------------------
	Environment mapping.
------------------------------------------------------------------------------*/

static void EnviroMap( FSceneNode* Frame, FTransTexture& P )
{
	FVector T = P.Point.UnsafeNormal().MirrorByVector( P.Normal ).TransformVectorBy( Frame->Uncoords );
	P.U = (T.X+1.0) * 0.5 * 256.0 * UScale;
	P.V = (T.Y+1.0) * 0.5 * 256.0 * VScale;
}

/*--------------------------------------------------------------------------
	Clippers.
--------------------------------------------------------------------------*/

static FLOAT Dot[32];
static inline INT Clip( FSceneNode* Frame, FTransTexture** Dest, FTransTexture** Src, INT SrcNum )
{
	INT DestNum=0;
	for( INT i=0,j=SrcNum-1; i<SrcNum; j=i++ )
	{
		if( Dot[j]>=0.0 )
		{
			Dest[DestNum++] = Src[j];
		}
		if( Dot[j]*Dot[i]<0.0 )
		{
			FTransTexture* T = Dest[DestNum] = New<FTransTexture>(GMem);
			*T = FTransTexture( *Src[j] + (*Src[i]-*Src[j]) * (Dot[j]/(Dot[j]-Dot[i])) );
			T->Project( Frame );
			DestNum++;
		}
	}
	return DestNum;
}

/*------------------------------------------------------------------------------
	Subsurface rendering.
------------------------------------------------------------------------------*/

// Triangle subdivision table.
static const int CutTable[8][4][3] =
{
	{{0,1,2},{9,9,9},{9,9,9},{9,9,9}},
	{{0,3,2},{2,3,1},{9,9,9},{9,9,9}},
	{{0,1,4},{4,2,0},{9,9,9},{9,9,9}},
	{{0,3,2},{2,3,4},{4,3,1},{9,9,9}},
	{{0,1,5},{5,1,2},{9,9,9},{9,9,9}},
	{{0,3,5},{5,3,1},{1,2,5},{9,9,9}},
	{{0,1,4},{4,2,5},{5,0,4},{9,9,9}},
	{{0,3,5},{3,1,4},{5,4,2},{3,4,5}}
};

void RenderSubsurface
(
	FSceneNode*		Frame,
	FTextureInfo&	Texture,
	FSpanBuffer*	Span,
	FTransTexture**	Pts,
	DWORD			PolyFlags,
	INT				SubCount
)
{
	guard(RenderSubsurface);

	// Handle effects.z
	if( PolyFlags & (PF_Environment | PF_Unlit) )
	{
		// Environment mapping.
		if( PolyFlags & PF_Environment )
			for( INT i=0; i<3; i++ )
				EnviroMap( Frame, *Pts[i] );

		// Handle unlit.
		if( PolyFlags & PF_Unlit )
			for( int j=0; j<3; j++ )
				Pts[j]->Light = GUnlitColor;
	}

	// Handle subdivision.
	if( SubCount<3 && !(PolyFlags & PF_Flat) )
	{
		// Compute side distances.
		INT CutSide[3], Cuts=0;
		FLOAT Alpha[3];
		STAT(uclock(GStat.MeshSubTime));
		for( INT i=0,j=2; i<3; j=i++ )
		{
			FLOAT Dist   = FDistSquared(Pts[j]->Point,Pts[i]->Point);
			FLOAT Curvy  = (Pts[j]->Normal ^ Pts[i]->Normal).SizeSquared();
			FLOAT Thresh = 50.0 * Frame->FX * SqrtApprox(Dist * Curvy) / Max(1.f, Pts[j]->Point.Z + Pts[i]->Point.Z);
			Alpha[j]     = Min( Thresh / Square(32.f) - 1.f, 1.f );
			CutSide[j]   = Alpha[j]>0.0;
			Cuts        += (CutSide[j]<<j);
		}
		STAT(uunclock(GStat.MeshSubTime));

		// See if it should be subdivided.
		if( Cuts )
		{
			STAT(uclock(GStat.MeshSubTime));
			FTransTexture Tmp[3];
			Pts[3]=Tmp+0;
			Pts[4]=Tmp+1;
			Pts[5]=Tmp+2;
			INT i;
			INT j=2;
			for( i=0; i<3; j=i++ ) if( CutSide[j] )
			{
				// Compute midpoint.
				FTransTexture& MidPt = *Pts[j+3];
				MidPt = (*Pts[j]+*Pts[i])*0.5;

				// Compute midpoint normal.
				MidPt.Normal = Pts[j]->Normal + Pts[i]->Normal;
				MidPt.Normal *= DivSqrtApprox( MidPt.Normal.SizeSquared() );

				// Enviro map it.
				if( PolyFlags & PF_Environment )
				{
					FLOAT U=MidPt.U, V=MidPt.V;
					EnviroMap( Frame, MidPt );
					MidPt.U = U + (MidPt.U - U)*Alpha[j];
					MidPt.V = V + (MidPt.V - V)*Alpha[j];
				}

				// Shade the midpoint.
				MidPt.Light += (GLightManager->Light( MidPt, PolyFlags ) - MidPt.Light) * Alpha[j];

				// Curve the midpoint.
				(FVector&)MidPt
				+=	0.15
				*	Alpha[j]
				*	(FVector&)MidPt.Normal
				*	SqrtApprox
					(
						(Pts[j]->Point  - Pts[i]->Point ).SizeSquared()
					*	(Pts[i]->Normal ^ Pts[j]->Normal).SizeSquared()
					);

				// Outcode and optionally transform midpoint.
				MidPt.ComputeOutcode( Frame );
				MidPt.Project( Frame );
			}
			FTransTexture* NewPts[6];
			for( i=0; i<4 && CutTable[Cuts][i][0]!=9; i++ )
			{
				for( INT j=0; j<3; j++ )
					NewPts[j] = Pts[CutTable[Cuts][i][j]];
				RenderSubsurface( Frame, Texture, Span, NewPts, PolyFlags, SubCount+1 );
			}
			STAT(uunclock(GStat.MeshSubTime));
			return;
		}
	}

	// If outcoded, skip it.
	if( Pts[0]->Flags & Pts[1]->Flags & Pts[2]->Flags )
		return;

	// Backface reject it.
	if( (PolyFlags & PF_TwoSided) && FTriple(Pts[0]->Point,Pts[1]->Point,Pts[2]->Point) <= 0.0 )
	{
		if( !(PolyFlags & PF_TwoSided) )
			return;
		Exchange( Pts[2], Pts[0] );
	}

	// Clip it.
	INT NumPts=3;
	BYTE AllCodes = Pts[0]->Flags | Pts[1]->Flags | Pts[2]->Flags;
	if( AllCodes )
	{
		if( AllCodes & FVF_OutXMin )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjXM * Pts[i]->Point.Z + Pts[i]->Point.X;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutXMax )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjXP * Pts[i]->Point.Z - Pts[i]->Point.X;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutYMin )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjYM * Pts[i]->Point.Z + Pts[i]->Point.Y;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( AllCodes & FVF_OutYMax )
		{
			static FTransTexture* LocalPts[8];
			for( INT i=0; i<NumPts; i++ )
				Dot[i] = Frame->PrjYP * Pts[i]->Point.Z - Pts[i]->Point.Y;
			NumPts = Clip( Frame, LocalPts, Pts, NumPts );
			if( NumPts==0 ) return;
			Pts = LocalPts;
		}
		if( Frame->NearClip.W != 0.0 )
		{
			UBOOL Clipped=0;
			for( INT i=0; i<NumPts; i++ )
			{
				Dot[i] = Frame->NearClip.PlaneDot(Pts[i]->Point);
				Clipped |= (Dot[i]<0.0);
			}
			if( Clipped )
			{
				static FTransTexture* LocalPts[8];
				NumPts = Clip( Frame, LocalPts, Pts, NumPts );
				if( NumPts==0 ) return;
				Pts = LocalPts;
			}
		}
	}

	for( INT i=0; i<NumPts; i++ )
	{
		//Pts[i]->ScreenX = Clamp(Pts[i]->ScreenX,0.f,Frame->FX);
		//Pts[i]->ScreenY = Clamp(Pts[i]->ScreenY,0.f,Frame->FY);
		ClipFloatFromZero(Pts[i]->ScreenX, Frame->FX);
		ClipFloatFromZero(Pts[i]->ScreenY, Frame->FY);
	}

	// Render it.
	STAT(uclock(GStat.MeshTmapTime));
	Frame->Viewport->RenDev->DrawGouraudPolygon( Frame, Texture, Pts, NumPts, PolyFlags, Span );
	STAT(uunclock(GStat.MeshTmapTime));
	STAT(GStat.MeshSubCount++);

	unguard;
}

/*------------------------------------------------------------------------------
	High level mesh rendering.
------------------------------------------------------------------------------*/

//
// Structure used by DrawMesh for sorting triangles.
//
struct FMeshTriSort
{
	FMeshTri* Tri;
	INT Key;
};
INT Compare( const FMeshTriSort& A, const FMeshTriSort& B )
{
	return B.Key - A.Key;
}
INT Compare( const FTransform* A, const FTransform* B )
{
	return appRound(B->Point.Z - A->Point.Z);
}

// Draw a mesh map.
//
void URender::DrawMesh
(
	FSceneNode*		Frame,
	AActor*			Owner,
	FSpanBuffer*	SpanBuffer,
	AZoneInfo*		Zone,
	const FCoords&	Coords,
	FVolActorLink*	LeafLights,
	FActorLink*		Volumetrics,
	DWORD			ExtraFlags
)
{
	guard(URender::DrawMesh);
	STAT(uclock(GStat.MeshTime));
	FMemMark Mark(GMem);
	UMesh*  Mesh = Owner->Mesh;
	FVector Hack = FVector(0,-8,0);
	UBOOL NotWeaponHeuristic=(Owner->Owner!=Frame->Viewport->Actor);
	if( !Engine->Client->CurvedSurfaces )
		ExtraFlags |= PF_Flat;

#if 0
	// For testing actor span clipping.
	if( SpanBuffer )
		for( INT i=SpanBuffer->StartY; i<SpanBuffer->EndY; i++ )
			for( FSpan* Span=SpanBuffer->Index[i-SpanBuffer->StartY]; Span; Span=Span->Next )
				appMemset( Frame->Screen(Span->Start,i), appRand(), (Span->End-Span->Start)*4 );
#endif

	// Get transformed verts.
	FTransTexture* Samples=NULL;
	UBOOL bWire=0;
	guardSlow(Transform);
	STAT(uclock(GStat.MeshGetFrameTime));
	Samples = New<FTransTexture>(GMem,Mesh->FrameVerts);
	bWire = Frame->Viewport->IsOrtho() || Frame->Viewport->Actor->RendMap==REN_Wire;
	Mesh->GetFrame( &Samples->Point, sizeof(Samples[0]), bWire ? GMath.UnitCoords : Coords, Owner );
	STAT(uunclock(GStat.MeshGetFrameTime));
	unguardSlow;

	// Compute outcodes.
	BYTE Outcode = FVF_OutReject;
	guardSlow(Outcode);
	STAT(uclock(GStat.MeshOutcodeTime));
	for( INT i=0; i<Mesh->FrameVerts; i++ )
	{
		Samples[i].Light.R = -1;
		Samples[i].ComputeOutcode( Frame );
		Outcode &= Samples[i].Flags;
	}
	STAT(uunclock(GStat.MeshOutcodeTime));
	unguardSlow;

	// Render a wireframe view or textured view.
	if( bWire )
	{
		// Render each wireframe triangle.
		guardSlow(RenderWire);
		FPlane Color = Owner->bSelected ? FPlane(.2,.8,.1,0) : FPlane(.6,.4,.1,0);
		for( INT i=0; i<Mesh->Tris.Num(); i++ )
		{
			FMeshTri& Tri    = Mesh->Tris(i);
			FVector*  P1     = &Samples[Tri.iVertex[2]].Point;
			for( int j=0; j<3; j++ )
			{
				FVector* P2 = &Samples[Tri.iVertex[j]].Point;
				if( (Tri.PolyFlags & PF_TwoSided) || P1->X>=P2->X  )
					Draw3DLine( Frame, Color, LINE_DepthCued, *P1, *P2 );
				P1 = P2;
			}
		}
		STAT(uunclock(GStat.MeshTime));
		Mark.Pop();
		unguardSlow;
		return;
	}

	// Coloring.
	FLOAT Unlit  = Clamp( Owner->ScaleGlow*0.5f + Owner->AmbientGlow/256.f, 0.f, 1.f );
	GUnlitColor  = FVector( Unlit, Unlit, Unlit );
	if( GIsEditor && (ExtraFlags & PF_Selected) )
		GUnlitColor = GUnlitColor*0.4 + FVector(0.0,0.6,0.0);

	// Mesh based particle effects.
	if( Owner->bParticles )
	{
		guardSlow(Particles);
		check(Owner->Texture);
		FTransform** SortedPts = New<FTransform*>(GMem,Mesh->FrameVerts);
		INT Count=0;
		INT i;
		for( i=0; i<Mesh->FrameVerts; i++ )
		{
			if( !Samples[i].Flags && Samples[i].Point.Z>1.0 )
			{
				Samples[i].Project( Frame );
				SortedPts[Count++] = &Samples[i];
			}
		}
		if( Frame->Viewport->RenDev->SpanBased )
		{
			appSort( SortedPts, Count );
		}
		for( i=0; i<Count; i++ )
		{
			if( !SortedPts[i]->Flags )
			{
				FLOAT XSize = SortedPts[i]->RZ * Owner->Texture->USize * Owner->DrawScale;
				FLOAT YSize = SortedPts[i]->RZ * Owner->Texture->VSize * Owner->DrawScale;
				Frame->Viewport->Canvas->DrawIcon
				(
					Owner->Texture,
					SortedPts[i]->ScreenX - XSize/2,
					SortedPts[i]->ScreenY - XSize/2,
					XSize,
					YSize,
					SpanBuffer,
					Samples[i].Point.Z,
					GUnlitColor,
					FPlane(0,0,0,0),
					ExtraFlags | PF_TwoSided | Owner->Texture->PolyFlags
				);
			}
		}
		Mark.Pop();
		STAT(uunclock(GStat.MeshTime));
		unguardSlow;
		return;
	}

	// Set up triangles.
	INT VisibleTriangles = 0;
	HasSpecialCoords = 0;
	FMeshTriSort* TriPool=NULL;
	FVector* TriNormals=NULL;
#ifdef __PSP__
	BYTE* TriNormalDone=NULL;
#endif
	if( Outcode == 0 )
	{
		// Process triangles.
		guardSlow(Process);
		TriPool    = New<FMeshTriSort>(GMem,Mesh->Tris.Num());
		TriNormals = New<FVector>(GMem,Mesh->Tris.Num());
#ifdef __PSP__
		// Triangle normals only feed vertex normals, which only feed lighting;
		// with the light cache below most frames need none, so they are
		// computed on demand (TriNormalDone) instead of for every triangle.
		TriNormalDone = New<BYTE>(GMem,Mesh->Tris.Num());
		appMemset( TriNormalDone, 0, Mesh->Tris.Num() );
#endif

		// Set up list for triangle sorting, adding all possibly visible triangles.
		STAT(uclock(GStat.MeshProcessTime));
		FMeshTriSort* TriTop = &TriPool[0];
		for( INT i=0; i<Mesh->Tris.Num(); i++ )
		{
			FMeshTri*   Tri = &Mesh->Tris(i);
			FTransform& V1  = Samples[Tri->iVertex[0]];
			FTransform& V2  = Samples[Tri->iVertex[1]];
			FTransform& V3  = Samples[Tri->iVertex[2]];
			DWORD PolyFlags = ExtraFlags | Tri->PolyFlags;

#ifndef __PSP__
			// Compute triangle normal.
			TriNormals[i] = (V1.Point-V2.Point) ^ (V3.Point-V1.Point);
			TriNormals[i] *= DivSqrtApprox(TriNormals[i].SizeSquared()+0.001);
#endif

			// See if potentially visible.
			if( !(V1.Flags & V2.Flags & V3.Flags) )
			{
				if
				(	(PolyFlags & (PF_TwoSided|PF_Flat|PF_Invisible))!=(PF_Flat)
				||	Frame->Mirror*FTriple(V1.Point,V2.Point,V3.Point)>0.0 )
				{
					// This is visible.
					TriTop->Tri = Tri;

					// Set the sort key.
					TriTop->Key
					= NotWeaponHeuristic ? appRound( V1.Point.Z + V2.Point.Z + V3.Point.Z )
					: TriTop->Key=appRound( FDistSquared(V1.Point,Hack)*FDistSquared(V2.Point,Hack)*FDistSquared(V3.Point,Hack) );

					// Add to list.
					VisibleTriangles++;
					TriTop++;
				}
			}
		}
		STAT(uunclock(GStat.MeshProcessTime));
		unguardSlow;
	}

	// Render triangles.
	if( VisibleTriangles>0 )
	{
		guardSlow(Render);

		// Fatness.
		UBOOL Fatten = Owner->Fatness!=128;
		FLOAT Fatness = (Owner->Fatness/16.0)-8.0;

		// Sort by depth.
		if( Frame->Viewport->RenDev->SpanBased )
			appSort( TriPool, VisibleTriangles );

		// Lock the textures.
		UTexture* EnvironmentMap = NULL;
		guardSlow(Lock);
		check(Mesh->Textures.Num()<=ARRAY_COUNT(TextureInfo));
		for( INT i=0; i<Mesh->Textures.Num(); i++ )
		{
			Textures[i] = Mesh->GetTexture( i, Owner );
			if( Textures[i] )
			{
				Textures[i]->GetInfo( TextureInfo[i], Frame->Viewport->CurrentTime );
				EnvironmentMap = Textures[i];
			}
		}
		if( Owner->Texture )
			EnvironmentMap = Owner->Texture;
		else if( Owner->Region.Zone && Owner->Region.Zone->EnvironmentMap )
			EnvironmentMap = Owner->Region.Zone->EnvironmentMap;
		else if( Owner->Level->EnvironmentMap )
			EnvironmentMap = Owner->Level->EnvironmentMap;
		check(EnvironmentMap);
		EnvironmentMap->GetInfo( EnvironmentInfo, Frame->Viewport->CurrentTime );
		unguardSlow;

		// Build list of all incident lights on the mesh.
		STAT(uclock(GStat.MeshLightSetupTime));
		ExtraFlags |= GLightManager->SetupForActor( Frame, Owner, LeafLights, Volumetrics );
		STAT(uunclock(GStat.MeshLightSetupTime));
#ifdef __PSP__
		// Per-actor vertex light cache. Without the specular term (dropped on
		// the PSP, see FLightManager::Light) a vertex's lighting depends only
		// on the actor's pose and its light set, not on the camera, so a
		// decoration that has not moved is lit once and read back afterwards
		// (this was 5 ms of a 19 ms mesh frame in the Vortex corridor; the
		// vertex and triangle normals it needed were another 3.5 ms).
		// Entries live in GCache under the actor; the key is compared whole.
		struct FPspMeshLightKey { UMesh* Mesh; INT Seq; FLOAT AnimFrame; FVector Loc; FRotator Rot; FLOAT Scale; DWORD Sig; DWORD Flags; INT Verts; };
		BYTE* LitCache = NULL; FCacheItem* LitItem = NULL;
		static INT UseLightCache = -1;
		if( UseLightCache < 0 ) { UseLightCache = 1; GetConfigInt( "PSP", "MeshLightCache", UseLightCache ); Parse( appCmdLine(), "MESHLIGHTCACHE=", UseLightCache ); }
		if( UseLightCache && !(ExtraFlags & (PF_RenderFog|PF_Unlit|PF_Selected)) && !Fatten && !GIsEditor )
		{
			FPspMeshLightKey Key; appMemset( &Key, 0, sizeof(Key) );
			Key.Mesh = Mesh; Key.Seq = Owner->AnimSequence.GetIndex(); Key.AnimFrame = Owner->AnimFrame; Key.Loc = Owner->Location + Owner->PrePivot; Key.Rot = Owner->Rotation;
			Key.Scale = Owner->DrawScale; Key.Sig = GLightManager->PspLightSignature(); Key.Flags = ExtraFlags & (PF_Unlit|PF_RenderFog); Key.Verts = Mesh->FrameVerts;
			const QWORD CID = MakeCacheID( CID_Extra0, Owner );
			BYTE* Mem = GCache.Get( CID, LitItem );
			if( Mem && appMemcmp( Mem, &Key, sizeof(Key) ) != 0 )
			{
				const FPspMeshLightKey& Old = *(FPspMeshLightKey*)Mem;
				STAT(GStat.MeshKeyMiss[ Old.Mesh!=Key.Mesh ? 0 : Old.Seq!=Key.Seq ? 1 : Old.AnimFrame!=Key.AnimFrame ? 2 : Old.Loc!=Key.Loc ? 3 : Old.Rot!=Key.Rot ? 4 : Old.Scale!=Key.Scale ? 5 : Old.Sig!=Key.Sig ? 6 : 7 ]++);
				LitItem->Unlock();
				GCache.Flush( CID );
				Mem = NULL;
			}
			else if( !Mem )
				STAT(GStat.MeshKeyMiss[8]++);
			if( !Mem )
			{
				Mem = GCache.Create( CID, LitItem, sizeof(Key) + Mesh->FrameVerts * 4 );
				appMemcpy( Mem, &Key, sizeof(Key) );
				appMemset( Mem + sizeof(Key), 0, Mesh->FrameVerts * 4 );
			}
			LitCache = Mem + sizeof(Key);
		}
#endif

		// Perform all vertex lighting.
		guardSlow(Light);
		for( INT i=0; i<VisibleTriangles; i++ )
		{
			FMeshTri& Tri = *TriPool[i].Tri;
			for( INT j=0; j<3; j++ )
			{
				INT iVert = Tri.iVertex[j];
				FTransSample& Vert = Samples[iVert];
				if( Vert.Light.R == -1 )
				{
#ifdef __PSP__
					DWORD* Slot = LitCache ? (DWORD*)LitCache + iVert : NULL;
					if( Slot && ( *Slot & 0xFF000000u ) )
					{
						Vert.Light = FPlane( ( *Slot & 0xff ) / 255.f, ( ( *Slot >> 8 ) & 0xff ) / 255.f, ( ( *Slot >> 16 ) & 0xff ) / 255.f, 0.f );
						Vert.Fog   = FPlane( 0, 0, 0, 0 );
						STAT(GStat.MeshVertsCached++);
						continue;
					}
#endif
					// Compute vertex normal.
					STAT(uclock(GStat.MeshNormalTime));
					FVector Norm(0,0,0);
					FMeshVertConnect& Connect = Mesh->Connects(iVert);
					for( INT k=0; k<Connect.NumVertTriangles; k++ )
					{
						const INT iTri = Mesh->VertLinks(Connect.TriangleListOffset + k);
#ifdef __PSP__
						if( !TriNormalDone[iTri] )
						{
							TriNormalDone[iTri] = 1;
							FMeshTri& T = Mesh->Tris(iTri);
							TriNormals[iTri] = (Samples[T.iVertex[0]].Point-Samples[T.iVertex[1]].Point) ^ (Samples[T.iVertex[2]].Point-Samples[T.iVertex[0]].Point);
							TriNormals[iTri] *= DivSqrtApprox(TriNormals[iTri].SizeSquared()+0.001);
						}
#endif
						Norm += TriNormals[iTri];
					}
					Vert.Normal = FPlane( Vert.Point, Norm * DivSqrtApprox(Norm.SizeSquared()) );

					// Fatten it if desired.
					if( Fatten )
					{
						Vert.Point += Vert.Normal * Fatness;
						Vert.ComputeOutcode( Frame );
					}
					STAT(uunclock(GStat.MeshNormalTime));

					// Compute effect of each lightsource on this vertex.
					STAT(uclock(GStat.MeshLightCalcTime));
					Vert.Light = GLightManager->Light( Vert, ExtraFlags );
					Vert.Fog   = GLightManager->Fog  ( Vert, ExtraFlags );
					STAT(uunclock(GStat.MeshLightCalcTime));
					STAT(GStat.MeshVertsLit++);
#ifdef __PSP__
					if( Slot )
						*Slot = (DWORD)Clamp( appRound( Vert.Light.R * 255.f ), 0, 255 ) | ( (DWORD)Clamp( appRound( Vert.Light.G * 255.f ), 0, 255 ) << 8 ) | ( (DWORD)Clamp( appRound( Vert.Light.B * 255.f ), 0, 255 ) << 16 ) | 0xFF000000u;
#else
					// Project it.
					STAT(uclock(GStat.MeshProjectTime));
					if( !Vert.Flags )
						Vert.Project( Frame );
					STAT(uunclock(GStat.MeshProjectTime));
#endif
				}
			}
		}
		unguardSlow;

		// Draw the triangles.
		guardSlow(DrawVisible);
		STAT(GStat.MeshPolyCount+=VisibleTriangles);
#ifdef __PSP__
		// Fast path: triangles whose three vertices are inside the view and
		// in front of the near plane need no clipping, so the driver takes
		// them as one list per texture and flag set (DrawMeshTris). Anything
		// else -- clipped, unlit, environment-mapped, invisible -- goes the
		// per-triangle way below. Marked by PF_Invisible in a scratch copy of
		// the flags so the loop below skips them.
		UBOOL* Taken = New<UBOOL>(GMem,VisibleTriangles);
		appMemset( Taken, 0, VisibleTriangles * sizeof(UBOOL) );
		STAT(uclock(GStat.MeshListTime));
		// Triangles need the CPU clipper only against the near plane: the GE
		// rasterises far outside the screen (its coordinate space is 4096
		// wide, the screen 480), so a vertex up to MeshGuardBand screen
		// half-widths outside still goes the fast way. 17% of a corridor's
		// triangles used to fall back, at 2 ms a frame.
		static FLOAT GuardBand = -1.f;
		if( GuardBand < 0.f ) { INT Pct = 300; GetConfigInt( "PSP", "MeshGuardBand", Pct ); Parse( appCmdLine(), "MESHGUARD=", Pct ); GuardBand = Clamp( Pct, 100, 700 ) / 100.f; }
		const FLOAT RProjZ = appTan( Frame->Viewport->Actor->FovAngle * PI / 360.0 );
		const FLOAT GBX = GuardBand * RProjZ, GBY = GuardBand * RProjZ * Frame->FY2 / Frame->FX2;
		if( !( Frame->NearClip.W == 0.0 && Frame->Mirror != -1 && !(ExtraFlags & PF_Environment) ) )
			STAT(GStat.MeshFallbackWhy[ Frame->NearClip.W != 0.0 ? 3 : Frame->Mirror == -1 ? 4 : 6 ] += VisibleTriangles);
		if( Frame->NearClip.W == 0.0 && Frame->Mirror != -1 && !(ExtraFlags & PF_Environment) )
		{
			const FMeshTri** List = New<const FMeshTri*>(GMem,VisibleTriangles);
			for( INT Tex=0; Tex<Mesh->Textures.Num(); Tex++ )
			{
				// distinct flag sets for this texture, at most a handful
				DWORD Seen[8]; INT NumSeen=0;
				for( INT i=0; i<VisibleTriangles; i++ )
				{
					FMeshTri& Tri = *TriPool[i].Tri;
					if( Tri.TextureIndex != Tex ) continue;
					DWORD PolyFlags = Tri.PolyFlags | ExtraFlags;
					INT k; for( k=0; k<NumSeen; k++ ) if( Seen[k]==PolyFlags ) break;
					if( k==NumSeen && NumSeen<8 ) Seen[NumSeen++]=PolyFlags;
				}
				for( INT s=0; s<NumSeen; s++ )
				{
					const DWORD PolyFlags = Seen[s];
					if( PolyFlags & (PF_Invisible|PF_Environment) ) { STAT(GStat.MeshFallbackWhy[2]++); continue; }
					INT Count=0;
					for( INT i=0; i<VisibleTriangles; i++ )
					{
						FMeshTri& Tri = *TriPool[i].Tri;
						if( Tri.TextureIndex != Tex || (Tri.PolyFlags | ExtraFlags) != PolyFlags ) continue;
						const FTransTexture& A = Samples[Tri.iVertex[0]];
						const FTransTexture& B = Samples[Tri.iVertex[1]];
						const FTransTexture& C = Samples[Tri.iVertex[2]];
						if( A.Point.Z<=1.f || B.Point.Z<=1.f || C.Point.Z<=1.f ) { STAT(GStat.MeshFallbackWhy[0]++); continue; }
						if( (A.Flags|B.Flags|C.Flags)
						&&	(	Abs(A.Point.X) > GBX*A.Point.Z || Abs(A.Point.Y) > GBY*A.Point.Z
							||	Abs(B.Point.X) > GBX*B.Point.Z || Abs(B.Point.Y) > GBY*B.Point.Z
							||	Abs(C.Point.X) > GBX*C.Point.Z || Abs(C.Point.Y) > GBY*C.Point.Z ) ) { STAT(GStat.MeshFallbackWhy[1]++); continue; }
						List[Count++] = &Tri; Taken[i] = 1;
					}
					if( !Count ) continue;
					// Unlit triangles take the actor's glow colour, as RenderSubsurface does.
					if( PolyFlags & PF_Unlit )
						for( INT i=0; i<Count; i++ )
							for( INT j=0; j<3; j++ )
								Samples[List[i]->iVertex[j]].Light = GUnlitColor;
					FTextureInfo& Info = Textures[Tex] ? TextureInfo[Tex] : EnvironmentInfo;
					const FLOAT US = Info.UScale * Info.USize / 256.0;
					const FLOAT VS = Info.VScale * Info.VSize / 256.0;
					STAT(uunclock(GStat.MeshListTime));
					STAT(uclock(GStat.MeshTmapTime));
					const UBOOL Ok = Frame->Viewport->RenDev->DrawMeshTris( Frame, Info, Samples, List, Count, PolyFlags, US, VS );
					STAT(uunclock(GStat.MeshTmapTime));
					STAT(uclock(GStat.MeshListTime));
					if( !Ok )
						for( INT i=0; i<VisibleTriangles; i++ ) if( Taken[i] && TriPool[i].Tri->TextureIndex==Tex && (TriPool[i].Tri->PolyFlags|ExtraFlags)==PolyFlags ) Taken[i]=0;
				}
			}
		}
		STAT(uunclock(GStat.MeshListTime));
		STAT(uclock(GStat.MeshFallbackTime));
#endif
		for( INT i=0; i<VisibleTriangles; i++ )
		{
			// Set up the triangle.
			FMeshTri& Tri = *TriPool[i].Tri;
#ifdef __PSP__
			if( Taken[i] )
				continue;
			STAT(GStat.MeshFallbackTris++);
#endif
			if( !(Tri.PolyFlags & PF_Invisible) )
			{
				// Get texture.
				DWORD PolyFlags = Tri.PolyFlags | ExtraFlags;
				INT Index = TriPool[i].Tri->TextureIndex;
				FTextureInfo& Info = (Textures[Index] && !(PolyFlags & PF_Environment)) ? TextureInfo[Index] : EnvironmentInfo;
				UScale = Info.UScale * Info.USize / 256.0;
				VScale = Info.VScale * Info.VSize / 256.0;

				// Set up texture coords.
				FTransTexture* Pts[6];
				for( INT j=0; j<3; j++ )
				{
					Pts[j]    = &Samples[Tri.iVertex[j]];
					Pts[j]->U = Tri.Tex[j].U * UScale;
					Pts[j]->V = Tri.Tex[j].V * VScale;
#ifdef __PSP__
					if( !Pts[j]->Flags )
						Pts[j]->Project( Frame );   // only the clipper needs screen coordinates
#endif
				}
				if( Frame->Mirror == -1 )
					Exchange( Pts[2], Pts[0] );
				RenderSubsurface( Frame, Info, SpanBuffer, Pts, PolyFlags, 0 );
			}
			else
			{
				// Remember coordinate system.
				FVector Mid = 0.5*(Samples[Tri.iVertex[0]].Point + Samples[Tri.iVertex[2]].Point);

				FCoords C;
				C.Origin = FVector(0,0,0);
				C.XAxis	 = (Samples[Tri.iVertex[1]].Point - Mid).SafeNormal();
				C.YAxis	 = (C.XAxis ^ (Samples[Tri.iVertex[0]].Point - Samples[Tri.iVertex[2]].Point)).SafeNormal();
				C.ZAxis	 = C.YAxis ^ C.XAxis;

				SpecialCoords = GMath.UnitCoords * Mid * C;
				HasSpecialCoords = 1;
			}
		}
#ifdef __PSP__
		STAT(uunclock(GStat.MeshFallbackTime));
		if( LitItem )
			LitItem->Unlock();
#endif
		GLightManager->FinishActor();
		unguardSlow;
		unguardSlow;
	}

	STAT(GStat.MeshCount++);
	STAT(uunclock(GStat.MeshTime));
	Mark.Pop();
	unguardf(( "(%s)", Owner->Mesh->GetName() ));
}

/*------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------*/
