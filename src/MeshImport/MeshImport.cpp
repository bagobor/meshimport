#pragma warning(disable:4702) // disabling a warning that only shows up when building VC7
#include <assert.h>
#include <vector>

#include "MeshImport.h"
#include "VtxWeld.h"
#include "MeshImportBuilder.h"
#include "UserMemAlloc.h"
#include "FileInterface.h"
#include "sutil.h"
#include "FloatMath.h"
#include "KeyValueIni.h"
#include "ExportFBX.h"

#pragma warning(disable:4996)


#ifdef WIN32
#ifdef MESHIMPORT_EXPORTS
#define MESHIMPORT_API __declspec(dllexport)
#else
#define MESHIMPORT_API __declspec(dllimport)
#endif
#else
#define MESHIMPORT_API
#endif

#pragma warning(disable:4100)

namespace NVSHARE
{

struct Vector
{
	NxF32 x;
	NxF32 y;
	NxF32 z;
};

struct Vertex
{
	NxU16 mIndex;
	NxF32 mTexel[2];
	NxU8  mMaterialIndex;
	NxU8  mUnused;
};

struct Triangle
{
	NxU16 mWedgeIndex[3];
	NxU8  mMaterialIndex;
	NxU8  mAuxMaterialIndex;
	NxU32 mSmoothingGroups;
};

struct Material
{
	char mMaterialName[64];
	NxI32 mTextureIndex;
	NxU32 mPolyFlags;
	NxU32 mAuxMaterial;
	NxU32 mAuxFlags;
	NxI32 mLodBias;
	NxI32 mLodStyle;
};

struct Bone
{
	char  mName[64];
	NxU32 mFlags;
	NxI32 mNumChildren;
	NxI32 mParentIndex;
	NxF32 mOrientation[4];
	NxF32 mPosition[3];
	NxF32 mLength;
	NxF32 mXSize;
	NxF32 mYSize;
	NxF32 mZSize;
};

struct BoneInfluence
{
	NxF32 mWeight;
	NxI32 mVertexIndex;
	NxI32 mBoneIndex;
};

struct AnimInfo
{
	char	mName[64];
	char	mGroup[64];    // Animation's group name
	NxI32	mTotalBones;           // TotalBones * NumRawFrames is number of animation keys to digest.
	NxI32	mRootInclude;          // 0 none 1 included 	(unused)
	NxI32	mKeyCompressionStyle;  // Reserved: variants in tradeoffs for compression.
	NxI32	mKeyQuotum;            // Max key quotum for compression
	NxF32	mKeyReduction;       // desired
	NxF32	mTrackTime;          // explicit - can be overridden by the animation rate
	NxF32	mAnimRate;           // frames per second.
	NxI32	mStartBone;            // - Reserved: for partial animations (unused)
	NxI32	mFirstRawFrame;        //
	NxI32	mNumRawFrames;         // NumRawFrames and AnimRate dictate tracktime...
};

struct AnimKey
{
	NxF32	mPosition[3];
	NxF32	mOrientation[4];
	NxF32	mTime;
};

struct ScaleKey
{
    NxF32 mScale[4];
};

}; // end of namespace

extern "C"
{
  MESHIMPORT_API NVSHARE::MeshImport * getInterface(NxI32 version_number);
};

namespace NVSHARE
{

class LocalVertexIndex : public VertexIndex, public Memalloc
{
public:
  LocalVertexIndex(NxF32 granularity)
  {
    mVertexIndex = fm_createVertexIndex(granularity,false);
  }

  virtual ~LocalVertexIndex(void)
  {
    fm_releaseVertexIndex(mVertexIndex);
  }

  virtual NxU32    getIndex(const NxF32 pos[3],bool &newPos)  // get welded index for this NxF32 vector[3]
  {
    return mVertexIndex->getIndex(pos,newPos);
  }

  virtual const NxF32 *   getVertices(void) const
  {
    return mVertexIndex->getVerticesFloat();
  }

  virtual const NxF32 *   getVertex(NxU32 index) const
  {
    return mVertexIndex->getVertexFloat(index);
  }

  virtual NxU32    getVcount(void) const
  {
    return mVertexIndex->getVcount();
  }

private:
  fm_VertexIndex *mVertexIndex;
};

class MyMeshImportApplicationResource : public MeshImportApplicationResource
{
public:
  virtual void * getApplicationResource(const char *base_name,const char *resource_name,NxU32 &len)
  {
    void * ret = 0;
    len = 0;

    FILE *fph = fopen(resource_name,"rb");
    if ( fph )
    {
      fseek(fph,0L,SEEK_END);
      len = ftell(fph);
      fseek(fph,0L,SEEK_SET);
      if ( len > 0 )
      {
        ret = MEMALLOC_MALLOC(len);
        fread(ret,len,1,fph);
      }
      fclose(fph);
    }
    return ret;
  }

  virtual void   releaseApplicationResource(void *mem)
  {
    MEMALLOC_FREE(mem);
  }

};

typedef std::vector< MeshImporter * > MeshImporterVector;

class MyMeshImport : public MeshImport, public MyMeshImportApplicationResource, public Memalloc
{
public:
  MyMeshImport(void)
  {
    mApplicationResource = this;
    NxU32 sections;
    mINI = loadKeyValueIni("MeshImportMaterialTranslate.ini",sections);
  }

  ~MyMeshImport(void)
  {
    releaseKeyValueIni(mINI);
  }

  VertexIndex *            createVertexIndex(NxF32 granularity)  // create an indexed vertext system for floats
  {
    LocalVertexIndex *m = MEMALLOC_NEW(LocalVertexIndex)(granularity);
    return static_cast< VertexIndex *>(m);
  }

  void                     releaseVertexIndex(VertexIndex *vindex)
  {
    LocalVertexIndex *m = static_cast< LocalVertexIndex *>(vindex);
    delete m;
  }

  virtual MeshImporter *   locateMeshImporter(const char *fname) // based on this file name, find a matching mesh importer.
  {
    MeshImporter *ret = 0;

    const char *dot = lastDot(fname);
    if ( dot )
    {
      char scratch[512];
      strncpy(scratch,dot,512);
      MeshImporterVector::iterator i;
      for (i=mImporters.begin(); i!=mImporters.end(); ++i)
      {
        MeshImporter *mi = (*i);
        NxI32 count = mi->getExtensionCount();
        for (NxI32 j=0; j<count; j++)
        {
          const char *ext = mi->getExtension(j);
          if ( stricmp(ext,scratch) == 0 )
          {
            ret = mi;
            break;
          }
        }
        if ( ret ) break;
      }
    }
    return ret;
  }

  virtual void addImporter(MeshImporter *importer)  // add an additional importer
  {
    if ( importer )
    {
      mImporters.push_back(importer);
    }
    else
    {
      printf("debug me");
    }
  }

  bool importMesh(const char *meshName,const void *data,NxU32 dlen,MeshImportInterface *callback,const char *options)
  {
    bool ret = false;

    MeshImporter *mi = locateMeshImporter(meshName);
    if ( mi )
    {
      ret = mi->importMesh(meshName,data,dlen,callback,options,mApplicationResource);
    }

    return ret;
  }

  virtual MeshSystem * getMeshSystem(MeshSystemContainer *_b)
  {
    MeshBuilder *b = (MeshBuilder *)_b;
    return static_cast< MeshSystem *>(b);
  }

  virtual MeshSystemContainer *     createMeshSystemContainer(void)
  {
    MeshSystemContainer *ret = 0;

    MeshBuilder *b = createMeshBuilder(mApplicationResource);
    if ( b )
    {
      ret = (MeshSystemContainer *)b;
    }

    return ret;
  }

  virtual MeshSystemContainer *     createMeshSystemContainer(const char *meshName,const void *data,NxU32 dlen,const char *options) // imports and converts to a single MeshSystem data structure
  {
    MeshSystemContainer *ret = 0;

    MeshImporter *mi = locateMeshImporter(meshName);
    if ( mi )
    {
      MeshBuilder *b = createMeshBuilder(mINI,meshName,data,dlen,mi,options,mApplicationResource);
      if ( b )
      {
        ret = (MeshSystemContainer *)b;
      }
    }

    return ret;
  }

  virtual void  releaseMeshSystemContainer(MeshSystemContainer *mesh)
  {
    MeshBuilder *b = (MeshBuilder *)mesh;
    releaseMeshBuilder(b);
  }

  virtual NxI32              getImporterCount(void)
  {
    return (NxI32)mImporters.size();
  }

  virtual MeshImporter    *getImporter(NxI32 index)
  {
    MeshImporter *ret = 0;
    assert( index >=0 && index < (NxI32)mImporters.size() );
    if ( index >= 0 && index < (NxI32)mImporters.size() )
    {
      ret = mImporters[index];
    }
    return ret;
  }

  const char *getStr(const char *str)
  {
    if ( str == 0 ) str = "";
    return str;
  }

  void printAABB(FILE_INTERFACE *fph,const MeshAABB &a)
  {
    fi_fprintf(fph,"       <MeshAABB min=\"%s,%s,%s\" max=\"%s,%s,%s\"/>\r\n", FloatString(a.mMin[0]), FloatString(a.mMin[1]), FloatString(a.mMin[2]), FloatString(a.mMax[0]), FloatString(a.mMax[1]), FloatString(a.mMax[2]) );
  }

  void print(FILE_INTERFACE *fph,MeshRawTexture *t)
  {
    assert(0); // not yet implemented
  }

  void print(FILE_INTERFACE *fph,MeshTetra *t)
  {
    assert(0); // not yet implemented
  }

  void print(FILE_INTERFACE *fph,MeshBone &b,MeshSkeleton *s)
  {
    const char *parent = 0;

    if ( b.mParentIndex >= 0 )
    {
      assert( b.mParentIndex >= 0 && b.mParentIndex < s->mBoneCount );
      if ( b.mParentIndex >= 0 &&  b.mParentIndex < s->mBoneCount )
      {
        parent = s->mBones[b.mParentIndex].mName;
      }
    }
    if ( parent )
    {
      fi_fprintf(fph,"        <Bone name=\"%s\" parent=\"%s\" orientation=\"%s %s %s %s\" position=\"%s %s %s\" scale=\"%s %s %s\"/>\r\n",
        b.mName,
        parent,
        FloatString(b.mOrientation[0]),
        FloatString(b.mOrientation[1]),
        FloatString(b.mOrientation[2]),
        FloatString(b.mOrientation[3]),
        FloatString(b.mPosition[0]),
        FloatString(b.mPosition[1]),
        FloatString(b.mPosition[2]),
        FloatString(b.mScale[0]),
        FloatString(b.mScale[1]),
        FloatString(b.mScale[2]) );
    }
    else
    {
      fi_fprintf(fph,"        <Bone name=\"%s\" orientation=\"%s %s %s %s\" position=\"%s %s %s\" scale=\"%s %s %s\"/>\r\n",
        b.mName,
        FloatString(b.mOrientation[0]),
        FloatString(b.mOrientation[1]),
        FloatString(b.mOrientation[2]),
        FloatString(b.mOrientation[3]),
        FloatString(b.mPosition[0]),
        FloatString(b.mPosition[1]),
        FloatString(b.mPosition[2]),
        FloatString(b.mScale[0]),
        FloatString(b.mScale[1]),
        FloatString(b.mScale[2]) );
    }
  }

  void print(FILE_INTERFACE *fph,MeshSkeleton *s)
  {
    fi_fprintf(fph,"      <Skeleton name=\"%s\" count=\"%d\">\r\n", s->mName, s->mBoneCount);
    for (NxU32 i=0; i<(NxU32)s->mBoneCount; i++)
    {
      print(fph,s->mBones[i],s);
    }
    fi_fprintf(fph,"      </Skeleton>\r\n");
  }

  void print(FILE_INTERFACE *fph,const MeshAnimPose &p)
  {
    fi_fprintf(fph,"      %s %s %s   %s %s %s %s   %s %s %s,\r\n", 
      FloatString(p.mPos[0]),
      FloatString(p.mPos[1]),
      FloatString(p.mPos[2]),
      FloatString(p.mQuat[0]),
      FloatString(p.mQuat[1]),
      FloatString(p.mQuat[2]),
      FloatString(p.mQuat[3]),
      FloatString(p.mScale[0]),
      FloatString(p.mScale[1]),
      FloatString(p.mScale[2]) );
  }

  void print(FILE_INTERFACE *fph,MeshAnimTrack *track)
  {
    fi_fprintf(fph,"        <AnimTrack name=\"%s\" count=\"%d\" has_scale=\"true\">\r\n", track->mName, track->mFrameCount);
    for (NxI32 i=0; i<track->mFrameCount; i++)
    {
      print(fph,track->mPose[i]);
    }
    fi_fprintf(fph,"        </AnimTrack>\r\n");

  }

  void print(FILE_INTERFACE *fph,MeshAnimation *a)
  {
    fi_fprintf(fph,"      <Animation name=\"%s\" trackcount=\"%d\" framecount=\"%d\" duration=\"%s\" dtime=\"%s\">\r\n", a->mName, a->mTrackCount, a->mFrameCount, FloatString( a->mDuration ), FloatString( a->mDtime) );

    for (NxI32 i=0; i<a->mTrackCount; i++)
    {
      print(fph,a->mTracks[i]);
    }

    fi_fprintf(fph,"      </Animation>\r\n");

  }

  void print(FILE_INTERFACE *fph,const MeshMaterial &m)
  {
    fi_fprintf(fph,"      <Material name=\"%s\" meta_data=\"%s\"/>\r\n", m.mName, m.mMetaData );
  }

  void print(FILE_INTERFACE *fph,MeshUserData *d)
  {
  }

  void print(FILE_INTERFACE *fph,MeshUserBinaryData *d)
  {
  }

  const char * getCtype(NxU32 flags)
  {
    mCtype.clear();

    if ( flags & MIVF_POSITION ) { mCtype+="fff ";  };
    if ( flags & MIVF_NORMAL ) { mCtype+="fff ";  };
    if ( flags & MIVF_COLOR ) { mCtype+="x4 ";  };
    if ( flags & MIVF_TEXEL1 ) { mCtype+="ff ";  };
    if ( flags & MIVF_TEXEL2 ) { mCtype+="ff ";  };
    if ( flags & MIVF_TEXEL3 ) { mCtype+="ff ";  };
    if ( flags & MIVF_TEXEL4 ) { mCtype+="ff ";  };
    if ( flags & MIVF_TANGENT ) { mCtype+="fff ";  };
    if ( flags & MIVF_BINORMAL ) { mCtype+="fff ";  };
    if ( flags & MIVF_BONE_WEIGHTING ) { mCtype+="ffff hhhh ";  };
    if ( flags & MIVF_INTERP1 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP2 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP3 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP4 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP5 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP6 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP7 ) { mCtype+="ffff "; };
    if ( flags & MIVF_INTERP8 ) { mCtype+="ffff "; };
    if ( flags & MIVF_RADIUS ) mCtype+="f ";

    if ( !mCtype.empty() )
    {
      char *foo = (char *)mCtype.c_str();
      NxI32 len =  (NxI32)strlen(foo);
      if ( foo[len-1] == ' ' )
      {
        foo[len-1] = 0;
      }
    }

    return mCtype.c_str();
  }

  const char * getSemantics(NxU32 flags)
  {
    mSemantic.clear();

    if ( flags & MIVF_POSITION ) { mSemantic+="position ";  };
    if ( flags & MIVF_NORMAL ) { mSemantic+="normal ";  };
    if ( flags & MIVF_COLOR ) { mSemantic+="color ";  };
    if ( flags & MIVF_TEXEL1 ) { mSemantic+="texcoord1 ";  };
    if ( flags & MIVF_TEXEL2 ) { mSemantic+="texcoord2 ";  };
    if ( flags & MIVF_TEXEL3 ) { mSemantic+="texcoord3 ";  };
    if ( flags & MIVF_TEXEL4 ) { mSemantic+="texcoord4 ";  };
    if ( flags & MIVF_TANGENT ) { mSemantic+="tangent ";  };
    if ( flags & MIVF_BINORMAL ) { mSemantic+="binormal ";  };
    if ( flags & MIVF_BONE_WEIGHTING ) { mSemantic+="blendweights blendindices ";  };
    if ( flags & MIVF_INTERP1 ) { mSemantic+="interp1 "; };
    if ( flags & MIVF_INTERP2 ) { mSemantic+="interp2 "; };
    if ( flags & MIVF_INTERP3 ) { mSemantic+="interp3 "; };
    if ( flags & MIVF_INTERP4 ) { mSemantic+="interp4 "; };
    if ( flags & MIVF_INTERP5 ) { mSemantic+="interp5 "; };
    if ( flags & MIVF_INTERP6 ) { mSemantic+="interp6 "; };
    if ( flags & MIVF_INTERP7 ) { mSemantic+="interp7 "; };
    if ( flags & MIVF_INTERP8 ) { mSemantic+="interp8 "; };
    if ( flags & MIVF_RADIUS ) mSemantic+="radius ";

    if ( !mSemantic.empty() )
    {
      char *foo = (char *)mSemantic.c_str();
      NxI32 len =  (NxI32)strlen(foo);
      if ( foo[len-1] == ' ' )
      {
        foo[len-1] = 0;
      }
    }


    return mSemantic.c_str();
  }

  void printVertex(FILE_INTERFACE *fph,NxU32 flags,const MeshVertex &v,NxU32 &column,bool &newRow)
  {
    if ( newRow )
    {
      if ( column != 0 )
      {
        fi_fprintf(fph,"\r\n");
      }
      newRow = false;
      column = 0;
      fi_fprintf(fph,"          ");
    }
    char scratch[1024] = { 0 };
    char temp[1024];

    if ( flags & MIVF_POSITION )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mPos[0]), FloatString(v.mPos[1]), FloatString(v.mPos[2]) );
      strcat(scratch,temp);
    }

    if ( flags & MIVF_NORMAL )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mNormal[0]), FloatString(v.mNormal[1]), FloatString(v.mNormal[2]) );
      strcat(scratch,temp);
    }

    if ( flags & MIVF_COLOR )
    {
      sprintf(temp,"%08X ", v.mColor );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_TEXEL1 )
    {
      sprintf(temp,"%s %s ", FloatString(v.mTexel1[0]), FloatString(v.mTexel1[1]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_TEXEL2 )
    {
      sprintf(temp,"%s %s ", FloatString(v.mTexel2[0]), FloatString(v.mTexel2[1]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_TEXEL3 )
    {
      sprintf(temp,"%s %s ", FloatString(v.mTexel3[0]), FloatString(v.mTexel3[1]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_TEXEL4 )
    {
      sprintf(temp,"%s %s ", FloatString(v.mTexel4[0]), FloatString(v.mTexel4[1]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_TANGENT )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mTangent[0]), FloatString(v.mTangent[1]), FloatString(v.mTangent[2]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_BINORMAL )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mBiNormal[0]), FloatString(v.mBiNormal[1]), FloatString(v.mBiNormal[2]) );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_BONE_WEIGHTING )
    {
      sprintf(temp,"%s %s %s %s ", FloatString(v.mWeight[0]), FloatString(v.mWeight[1]), FloatString(v.mWeight[2]), FloatString(v.mWeight[3]) );
      strcat(scratch,temp);
      sprintf(temp,"%d %d %d %d ", v.mBone[0], v.mBone[1], v.mBone[2], v.mBone[3] );
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP1 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp1[0]), FloatString(v.mInterp1[1]), FloatString(v.mInterp1[2]) , FloatString(v.mInterp1[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP2 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp2[0]), FloatString(v.mInterp2[1]), FloatString(v.mInterp2[2]) , FloatString(v.mInterp2[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP3 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp3[0]), FloatString(v.mInterp3[1]), FloatString(v.mInterp3[2]) , FloatString(v.mInterp3[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP4 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp4[0]), FloatString(v.mInterp4[1]), FloatString(v.mInterp4[2]) , FloatString(v.mInterp4[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP5 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp5[0]), FloatString(v.mInterp5[1]), FloatString(v.mInterp5[2]) , FloatString(v.mInterp5[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP6 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp6[0]), FloatString(v.mInterp6[1]), FloatString(v.mInterp6[2]) , FloatString(v.mInterp6[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP7 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp7[0]), FloatString(v.mInterp7[1]), FloatString(v.mInterp7[2]) , FloatString(v.mInterp7[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_INTERP8 )
    {
      sprintf(temp,"%s %s %s ", FloatString(v.mInterp8[0]), FloatString(v.mInterp8[1]), FloatString(v.mInterp8[2]) , FloatString(v.mInterp8[3]));
      strcat(scratch,temp);
    }
    if ( flags & MIVF_RADIUS )
    {
      sprintf(temp,"%s ", FloatString(v.mRadius) );
      strcat(scratch,temp);
    }
    strcat(scratch,",    ");
    NxU32 slen = (NxI32)strlen(scratch);
    fi_fprintf(fph,"%s", scratch );
    column+=slen;
    if ( column >= 160 )
      newRow = true;
  }

    void printIndex(FILE_INTERFACE *fph,const NxU32 *idx,NxU32 &column,bool &newRow)
  {
    if ( newRow )
    {
      if ( column != 0 )
      {
        fi_fprintf(fph,"\r\n");
      }
      newRow = false;
      column = 0;
      fi_fprintf(fph,"          ");
    }
    char temp[1024];
    sprintf(temp,"%d %d %d,  ", idx[0], idx[1], idx[2] );
    fi_fprintf(fph,"%s",temp);
    NxU32 slen = (NxU32)strlen(temp);
    column+=slen;
    if ( column >= 160 )
      newRow = true;
  }


  void print(FILE_INTERFACE *fph,SubMesh *m)
  {
    fi_fprintf(fph,"      <MeshSection material=\"%s\" ctype=\"%s\" semantic=\"%s\">\r\n", m->mMaterialName, getCtype(m->mVertexFlags), getSemantics(m->mVertexFlags) );
    printAABB(fph,m->mAABB);

    fi_fprintf(fph,"        <indexbuffer triangle_count=\"%d\">\r\n", m->mTriCount );
    const NxU32 *scan = m->mIndices;
    bool newRow = true;
    NxU32 column = 0;
    for (NxU32 i=0; i<m->mTriCount; i++)
    {
      printIndex(fph,scan,column,newRow);
      scan+=3;
    }
    fi_fprintf(fph,"\r\n");
    fi_fprintf(fph,"        </indexbuffer>\r\n");

    fi_fprintf(fph,"      </MeshSection>\r\n");
  }

  void print(FILE_INTERFACE *fph,Mesh *m)
  {
    fi_fprintf(fph,"      <Mesh name=\"%s\" skeleton=\"%s\" submesh_count=\"%d\">\r\n", m->mName, m->mSkeletonName, m->mSubMeshCount );
    printAABB(fph,m->mAABB);

    fi_fprintf(fph,"        <vertexbuffer count=\"%d\" ctype=\"%s\" semantic=\"%s\">\r\n", m->mVertexCount, getCtype(m->mVertexFlags), getSemantics(m->mVertexFlags) );

    bool newRow=true;
    NxU32 column=0;

    for (NxU32 i=0; i<m->mVertexCount; i++)
    {
      printVertex(fph, m->mVertexFlags, m->mVertices[i], column, newRow );

    }
    fi_fprintf(fph,"\r\n");
    fi_fprintf(fph,"        </vertexbuffer>\r\n");


    for (NxU32 i=0; i<m->mSubMeshCount; i++)
    {
      print(fph,m->mSubMeshes[i]);
    }

    fi_fprintf(fph,"      </Mesh>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshInstance &m)
  {
    fi_fprintf(fph,"        <MeshInstance mesh=\"%s\" position=\"%s,%s,%s\" rotation=\"%s,%s,%s,%s\" scale=\"%s,%s,%s\"/>\r\n",
      m.mMeshName,
      FloatString( m.mPosition[0] ),
      FloatString( m.mPosition[1] ),
      FloatString( m.mPosition[2] ),
      FloatString( m.mRotation[0] ),
      FloatString( m.mRotation[1] ),
      FloatString( m.mRotation[2] ),
      FloatString( m.mRotation[3] ),
      FloatString( m.mScale[0] ),
      FloatString( m.mScale[1] ),
      FloatString( m.mScale[2] ) );
  }

  const char * getTypeString(MeshCollisionType t)
  {
    const char *ret = "unknown";
    switch ( t )
    {
      case MCT_BOX: ret = "BOX"; break;
      case MCT_SPHERE: ret = "SPHERE"; break;
      case MCT_CAPSULE: ret = "CAPSULE"; break;
      case MCT_CONVEX: ret = "CONVEX"; break;
    }
    return ret;
  }

  void print(FILE_INTERFACE *fph,MeshCollisionBox *b)
  {
    fi_fprintf(fph,"       <MeshCollisionBox >\r\n");
    fi_fprintf(fph,"       </MeshCollisionBox>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshCollisionSphere *b)
  {
    fi_fprintf(fph,"       <MeshCollisionSphere >\r\n");
    fi_fprintf(fph,"       </MeshCollisionSphere>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshCollisionCapsule *b)
  {
    fi_fprintf(fph,"       <MeshCollisionCapsule >\r\n");
    fi_fprintf(fph,"       </MeshCollisionCapsule>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshCollisionConvex *m)
  {
    fi_fprintf(fph,"       <MeshCollisionConvex >\r\n");

    {
      bool newRow = true;
      fi_fprintf(fph,"         <vertexbuffer count=\"%d\" ctype=\"fff\" semantic=\"position\">\r\n", m->mVertexCount );
      for (NxU32 i=0; i<m->mVertexCount; i++)
      {
        const NxF32 *p = &m->mVertices[i*3];
        if ( newRow )
        {
          fi_fprintf(fph,"          ");
          newRow = false;
        }

        fi_fprintf(fph,"%s %s %s, ",FloatString(p[0]), FloatString(p[1]), FloatString(p[2]) );
        if ( (i&7) == 0 )
        {
          fi_fprintf(fph,"\r\n");
          newRow = true;
        }
      }
      if ( !newRow )
        fi_fprintf(fph,"\r\n");
      fi_fprintf(fph,"        </vertexbuffer>\r\n");
    }

    {
      fi_fprintf(fph,"         <indexbuffer triangle_count=\"%d\">\r\n", m->mTriCount );
      const NxU32 *scan = m->mIndices;
      bool newRow = true;
      NxU32 column = 0;
      for (NxU32 i=0; i<m->mTriCount; i++)
      {
        printIndex(fph,scan,column,newRow);
        scan+=3;
      }
      fi_fprintf(fph,"\r\n");
      fi_fprintf(fph,"        </indexbuffer>\r\n");
    }

    fi_fprintf(fph,"       </MeshCollisionConvex>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshCollision *m)
  {
    fi_fprintf(fph,"        <MeshCollision name=\"%s\" type=\"%s\" transform=\"%s %s %s %s   %s %s %s %s   %s %s %s %s   %s %s %s %s\">\r\n",
      m->mName,getTypeString(m->mType),
      FloatString( m->mTransform[0] ),
      FloatString( m->mTransform[1] ),
      FloatString( m->mTransform[2] ),
      FloatString( m->mTransform[3] ),
      FloatString( m->mTransform[4] ),
      FloatString( m->mTransform[5] ),
      FloatString( m->mTransform[6] ),
      FloatString( m->mTransform[7] ),
      FloatString( m->mTransform[8] ),
      FloatString( m->mTransform[9] ),
      FloatString( m->mTransform[10] ),
      FloatString( m->mTransform[11] ),
      FloatString( m->mTransform[12] ),
      FloatString( m->mTransform[13] ),
      FloatString( m->mTransform[14] ),
      FloatString( m->mTransform[15] ) );

    switch ( m->mType )
    {
      case MCT_BOX:
        {
          MeshCollisionBox *b = static_cast< MeshCollisionBox *>(m);
          print(fph,b);
        }
        break;
      case MCT_SPHERE:
        {
          MeshCollisionSphere *b = static_cast< MeshCollisionSphere *>(m);
          print(fph,b);
        }
        break;
      case MCT_CAPSULE:
        {
          MeshCollisionCapsule *b = static_cast< MeshCollisionCapsule *>(m);
          print(fph,b);
        }
        break;
      case MCT_CONVEX:
        {
          MeshCollisionConvex *b = static_cast< MeshCollisionConvex *>(m);
          print(fph,b);
        }
        break;
    }

    fi_fprintf(fph,"        </MeshCollision>\r\n");
  }

  void print(FILE_INTERFACE *fph,MeshCollisionRepresentation *m)
  {
    fi_fprintf(fph,"      <MeshCollisionRepresentation name=\"%s\" info=\"%s\" count=\"%d\">\r\n", m->mName, m->mInfo, m->mCollisionCount );
    for (NxU32 i=0; i<m->mCollisionCount; i++)
    {
      print(fph,m->mCollisionGeometry[i]);
    }
    fi_fprintf(fph,"      </MeshCollisionRepresentation>\r\n");

  }

  void putHeader(FILE_INTERFACE *fph,const char *header,NxI32 type,NxI32 len,NxI32 count)
  {
	  char h[20];
	  strncpy(h,header,20);
	  fi_fwrite(h,sizeof(h),1,fph);
	  fi_fwrite(&type, sizeof(NxI32), 1, fph );
	  fi_fwrite(&len, sizeof(NxI32), 1, fph );
	  fi_fwrite(&count, sizeof(NxI32), 1, fph );
  }

  void serializePSK(FILE_INTERFACE *fph,MeshSystem *ms,FILE_INTERFACE *fpanim)
  {
	  if ( ms->mMeshCount == 0 ) return;

	  typedef std::vector< Vector > VectorVector;
	  typedef std::vector< Vertex > VertexVector;
	  typedef std::vector< Triangle > TriangleVector;
	  typedef std::vector< Material > MaterialVector;
	  typedef std::vector< Bone > BoneVector;
	  typedef std::vector< BoneInfluence > BoneInfluenceVector;

	  Mesh *mesh = ms->mMeshes[0];

	  VectorVector _positions;
	  VertexVector _vertices;
	  TriangleVector _triangles;
	  MaterialVector _materials;
	  BoneVector _bones;
	  BoneInfluenceVector _boneInfluences;
	  const NxF32 POSITION_SCALE=50.0f;

	  for (NxU32 i=0; i<mesh->mVertexCount; i++)
	  {
		  Vector v;
		  MeshVertex &mv = mesh->mVertices[i];
		  v.x = mv.mPos[0]*POSITION_SCALE;
		  v.y = mv.mPos[1]*POSITION_SCALE;
		  v.z = mv.mPos[2]*POSITION_SCALE;;
		  _positions.push_back(v);
	  }

	  for (NxU32 i=0; i<mesh->mVertexCount; i++)
	  {
		  //		  NxU16 mIndex;
		  //		  NxF32 mTexel[2];
		  //		  NxU8  mMaterialIndex;
		  //		  NxU8  mUnused;

		  Vertex v;
		  MeshVertex &mv = mesh->mVertices[i];

		  v.mIndex = (NxU16)i;
		  v.mTexel[0] = mv.mTexel1[0];
		  v.mTexel[1] = mv.mTexel1[1];
		  v.mMaterialIndex = 0;
		  v.mUnused = 0;

		  _vertices.push_back(v);
	  }


	  for (NxU32 i=0; i<mesh->mSubMeshCount; i++)
	  {
		  SubMesh &sm = *mesh->mSubMeshes[i];
		  NxU32 matid = 0;
		  for (NxU32 k=0; k<ms->mMaterialCount; k++)
		  {
			  if ( stricmp(sm.mMaterialName,ms->mMaterials[k].mName) == 0 )
			  {
				  matid = k;
				  break;
			  }
		  }
		  for (NxU32 j=0; j<sm.mTriCount; j++)
		  {
			  NxU32 i1 = sm.mIndices[j*3+0];
			  NxU32 i2 = sm.mIndices[j*3+1];
			  NxU32 i3 = sm.mIndices[j*3+2];

			  Vertex &v1  = _vertices[i1];
			  Vertex &v2  = _vertices[i2];
			  Vertex &v3  = _vertices[i3];

			  v1.mMaterialIndex = (NxU8)matid;
			  v2.mMaterialIndex = (NxU8)matid;
			  v3.mMaterialIndex = (NxU8)matid;

//			  NxU16 mWedgeIndex[3];
//			  NxU8  mMaterialIndex;
//			  NxU8  mAuxMaterialIndex;
//			  NxU32 mSmoothingGroups;

			  Triangle t;

			  t.mWedgeIndex[0] = (NxU16)i3;
			  t.mWedgeIndex[1] = (NxU16)i2;
			  t.mWedgeIndex[2] = (NxU16)i1;

			  t.mMaterialIndex = (NxU8)matid;
			  t.mAuxMaterialIndex = 0;
			  t.mSmoothingGroups = 0;
			  _triangles.push_back(t);

		  }
	  }

	  for (NxU32 i=0; i<ms->mMaterialCount; i++)
	  {
//		  char mMaterialName[64];
//		  NxI32 mTextureIndex;
//		  NxU32 mPolyFlags;
//		  NxU32 mAuxMaterial;
//		  NxI32 mLodBias;
//		  NxI32 mLodStyle;

		  MeshMaterial &mm = ms->mMaterials[i];
		  Material m;
		  strncpy(m.mMaterialName,mm.mName,64);
		  m.mTextureIndex = 0;
		  m.mPolyFlags = 0;
		  m.mAuxMaterial = 0;
		  m.mLodBias = 0;
		  m.mLodStyle = 5;
		  _materials.push_back(m);
	  }

	  if ( ms->mSkeletonCount )
	  {
		  MeshSkeleton &sk = *ms->mSkeletons[0];
		  for (NxI32 i=0; i<sk.mBoneCount; i++)
		  {
//			  char  mName[64];
//			  NxU32 mFlags;
//			  NxI32 mNumChildren;
//			  NxI32 mParentIndex;
//			  NxF32 mOrientation[4];
//			  NxF32 mPosition[3];
//			  NxF32 mLength;
//			  NxF32 mXSize;
//			  NxF32 mYSize;
//			  NxF32 mZSize;

			  MeshBone &mb = sk.mBones[i];
			  Bone b;
			  strncpy(b.mName, mb.mName, 64 );
			  b.mParentIndex = (mb.mParentIndex == -1) ? 0 : mb.mParentIndex;

			  b.mOrientation[0] = mb.mOrientation[0];
			  b.mOrientation[1] = mb.mOrientation[1];
			  b.mOrientation[2] = mb.mOrientation[2];
			  b.mOrientation[3] = mb.mOrientation[3];

			  if ( i )
			  {
				b.mOrientation[3]*=-1;
			  }

			  b.mPosition[0] = mb.mPosition[0]*POSITION_SCALE;
			  b.mPosition[1] = mb.mPosition[1]*POSITION_SCALE;
			  b.mPosition[2] = mb.mPosition[2]*POSITION_SCALE;

			  b.mNumChildren = 0;

			  if ( mb.mParentIndex != -1 )
			  {
				  b.mLength = fm_distance( mb.mPosition, sk.mBones[ mb.mParentIndex ].mPosition)*POSITION_SCALE;
			  }
			  for (NxI32 k=i+1; k<sk.mBoneCount; k++)
			  {
				  MeshBone &ch = sk.mBones[k];
				  if ( ch.mParentIndex == i )
				  {
					  b.mNumChildren++;
				  }
			  }

			  b.mXSize = mb.mScale[0];
			  b.mYSize = mb.mScale[1];
			  b.mZSize = mb.mScale[2];

			  _bones.push_back(b);

		  }
	  }

	  for (NxU32 i=0; i<mesh->mVertexCount; i++)
	  {

		  BoneInfluence b;
		  MeshVertex &mv = mesh->mVertices[i];

//		  NxF32 mWeight;
//		  NxI32 mVertexIndex;
//		  NxI32 mBoneIndex;
		  b.mVertexIndex = i;
		  for (NxU32 j=0; j<4; j++)
		  {
			  if ( mv.mWeight[j] >0 )
			  {
				  b.mWeight = mv.mWeight[j];
				  b.mBoneIndex = mv.mBone[j];
				  _boneInfluences.push_back(b);
			  }
		  }
	  }





	  NxI32 positionsCount			= (NxI32)_positions.size();
	  Vector *positions				= positionsCount ? &_positions[0] : 0;

	  NxI32 vertexCount				= (NxI32) _vertices.size();
	  Vertex *vertices				= vertexCount ? &_vertices[0] : 0;

	  NxI32 triangleCount			= (NxI32) _triangles.size();
	  Triangle *triangles			= triangleCount ? &_triangles[0] : 0;

	  NxI32 materialCount			= (NxI32) _materials.size();
	  Material *materials			= materialCount ? &_materials[0] : 0;

	  NxI32 boneCount				= (NxI32)_bones.size();
	  Bone *bones					= boneCount ? &_bones[0] : 0;

	  NxI32 boneInfluenceCount		= (NxI32)_boneInfluences.size();
	  BoneInfluence *boneInfluences = boneInfluenceCount ? &_boneInfluences[0] : 0;

	  putHeader(fph,"ACTRHEAD",2003321,0,0);

	  putHeader(fph,"PNTS0000",0,sizeof(Vector),positionsCount);
	  if ( positionsCount ) fi_fwrite(positions,sizeof(Vector)*positionsCount,1,fph);

	  putHeader(fph,"VTXW0000",0,sizeof(Vertex),vertexCount);
	  if ( vertexCount ) fi_fwrite(vertices, sizeof(Vertex)*vertexCount,1,fph);

	  putHeader(fph,"FACE0000",0,sizeof(Triangle),triangleCount);
	  if ( triangleCount ) fi_fwrite(triangles,sizeof(Triangle)*triangleCount,1,fph);

	  putHeader(fph,"MATT0000",0,sizeof(Material),materialCount);
	  if ( materialCount ) fi_fwrite(materials,sizeof(Material)*materialCount,1,fph);

	  putHeader(fph,"REFSKELT",0,sizeof(Bone),boneCount);
	  if ( boneCount ) fi_fwrite(bones,sizeof(Bone)*boneCount,1,fph);

	  putHeader(fph,"RAWWEIGHTS",0,sizeof(BoneInfluence),boneInfluenceCount);
	  if ( boneInfluenceCount ) fi_fwrite(boneInfluences,sizeof(BoneInfluence)*boneInfluenceCount,1,fph);


      // ok let's output the animation data if there is any..
      if ( ms->mAnimationCount )
      {


        //struct AnimInfo
        //{
        //	char	mName[64];
        //	char	mGroup[64];    // Animation's group name
        //	NxI32	mTotalBones;           // TotalBones * NumRawFrames is number of animation keys to digest.
        //	NxI32	mRootInclude;          // 0 none 1 included 	(unused)
        //	NxI32	mKeyCompressionStyle;  // Reserved: variants in tradeoffs for compression.
        //	NxI32	mKeyQuotum;            // Max key quotum for compression
        //	NxF32	mKeyReduction;       // desired
        //	NxF32	mTrackTime;          // explicit - can be overridden by the animation rate
        //	NxF32	mAnimRate;           // frames per second.
        //	NxI32	mStartBone;            // - Reserved: for partial animations (unused)
        //	NxI32	mFirstRawFrame;        //
        //	NxI32	mNumRawFrames;         // NumRawFrames and AnimRate dictate tracktime...
        //};

        MeshAnimation *anim = ms->mAnimations[0];

        AnimInfo ainfo;
        strncpy(ainfo.mName, anim->mName, 64 );
        strcpy(ainfo.mGroup,"NONE");
        ainfo.mTotalBones = anim->mTrackCount;
        ainfo.mRootInclude = 0;
        ainfo.mKeyCompressionStyle = 0;
		ainfo.mKeyQuotum = 55088;
		ainfo.mKeyReduction = 1;
        ainfo.mTrackTime = (NxF32)(anim->mFrameCount); // 
        ainfo.mAnimRate = 1.0f / anim->mDtime;
        ainfo.mFirstRawFrame = 0;
		ainfo.mStartBone = 0;
		ainfo.mFirstRawFrame = 0;
        ainfo.mNumRawFrames = anim->mTrackCount;

        typedef std::vector< AnimKey > AnimKeyVector;
        typedef std::vector< Bone > BoneVector;
        BoneVector animBones;

        MeshSkeleton *sk = 0;
        if ( ms->mSkeletonCount )
        {
            sk = ms->mSkeletons[0];
            if ( sk->mBoneCount != anim->mTrackCount )
            {
                sk = 0;
            }
        }

        for (NxI32 i=0; i<anim->mTrackCount; i++)
        {
            MeshAnimTrack *track = anim->mTracks[i];
            MeshAnimPose  &pose  = track->mPose[0];
            Bone b;
            strncpy(b.mName, track->mName, 64 );
            b.mFlags = 0;
            b.mNumChildren = 0;
            b.mParentIndex = 0;

            if ( sk )
            {
                MeshBone &mb = sk->mBones[i];
                if ( strcmp(mb.mName,b.mName) == 0 )
                {
                    b = _bones[i]; // just copy the bone from the skeleton.
                }
            }

            b.mPosition[0] = pose.mPos[0];
            b.mPosition[1] = -pose.mPos[1];
            b.mPosition[2] = pose.mPos[2];

            b.mOrientation[0] = pose.mQuat[0];
            b.mOrientation[1] = -pose.mQuat[1];
            b.mOrientation[2] = pose.mQuat[2];
            b.mOrientation[3] = -pose.mQuat[3];

            if ( i > 0 )
            {
                b.mOrientation[3]*=-1;
            }

            b.mXSize = pose.mScale[0];
            b.mYSize = pose.mScale[1];
            b.mZSize = pose.mScale[2];

            if ( i )
            {

              b.mLength = fm_distance( b.mPosition, animBones[ b.mParentIndex ].mPosition );
            }
            else
            {
                b.mLength = 0;
            }

            animBones.push_back(b);
        }

        AnimKeyVector keys;
        NxF32 ctime = 0;
        for (NxI32 i=0; i<anim->mFrameCount; i++)
        {
            for (NxI32 j=0; j<anim->mTrackCount; j++)
            {
                MeshAnimTrack *track = anim->mTracks[j];
                MeshAnimPose  &pose  = track->mPose[i];
                AnimKey a;

                a.mPosition[0] = pose.mPos[0];
                a.mPosition[1] = pose.mPos[1];
                a.mPosition[2] = pose.mPos[2];

                a.mOrientation[0] = pose.mQuat[0];
                a.mOrientation[1] = pose.mQuat[1];
                a.mOrientation[2] = pose.mQuat[2];
                a.mOrientation[3] = pose.mQuat[3];
                a.mTime = ctime;

                if ( j )
                {
                    a.mOrientation[3]*=-1;
                }
                keys.push_back(a);
            }
            ctime+=anim->mDtime;
        }


    	NxI32 boneCount				= (NxI32)animBones.size();
    	Bone *bones					= boneCount ? &animBones[0] : 0;
        NxI32 keyCount              = (NxI32)keys.size();
        AnimKey *k                  = keyCount ? &keys[0] : 0;


        putHeader(fpanim,"ANIMHEAD",2003321,0,0);

        putHeader(fpanim,"BONENAMES", 0, sizeof(Bone), anim->mTrackCount);
 	    if ( boneCount ) fi_fwrite(bones,sizeof(Bone)*boneCount,1,fpanim);

        putHeader(fpanim,"ANIMINFO",  0, sizeof(AnimInfo), 1 );
        fi_fwrite(&ainfo, sizeof(AnimInfo), 1, fpanim );

        putHeader(fpanim,"ANIMKEYS",  0, sizeof(AnimKey), keyCount );
        if ( keyCount ) fi_fwrite(k, sizeof(AnimKey)*keyCount, 1, fpanim);

        putHeader(fpanim,"SCALEKEYS", 0, sizeof(ScaleKey), 0 );


      }



  }

  void serializeEzm(FILE_INTERFACE *fph,MeshSystem *mesh)
  {
    fi_fprintf(fph,"<?xml version=\"1.0\"?>\r\n");
    fi_fprintf(fph,"  <MeshSystem asset_name=\"%s\" asset_info=\"%s\" mesh_system_version=\"%d\" mesh_system_asset_version=\"%d\">\r\n", getStr(mesh->mAssetName), getStr(mesh->mAssetInfo), mesh->mMeshSystemVersion, mesh->mAssetVersion );
    printAABB(fph,mesh->mAABB);

    //*******************************************************************
    //***
    //***  Output Textures
    //***
    //*******************************************************************
    if ( mesh->mTextureCount )
    {
      fi_fprintf(fph,"    <Textures count=\"%d\">\r\n", mesh->mTextureCount );
      for (NxU32 i=0; i<mesh->mTextureCount; i++)
      {
        print(fph,mesh->mTextures[i]);
      }
      fi_fprintf(fph,"    </Textures>\r\n");
    }


    //*******************************************************************
    //***
    //***  Tetraheadral meshes
    //***
    //*******************************************************************
    if ( mesh->mTetraMeshCount )
    {
      fi_fprintf(fph,"    <TetraMeshes count=\"%d\">\r\n", mesh->mTetraMeshCount );
      for (NxU32 i=0; i<mesh->mTetraMeshCount; i++)
      {
        print(fph,mesh->mTetraMeshes[i]);
      }
      fi_fprintf(fph,"    </TetraMeshes>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output skeletons
    //***
    //*******************************************************************
    if ( mesh->mSkeletonCount )
    {
      fi_fprintf(fph,"    <Skeletons count=\"%d\">\r\n", mesh->mSkeletonCount);
      for (NxU32 i=0; i<mesh->mSkeletonCount; i++)
      {
        print(fph,mesh->mSkeletons[i]);
      }
      fi_fprintf(fph,"    </Skeletons>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output Animations
    //***
    //*******************************************************************
    if ( mesh->mAnimationCount )
    {
      fi_fprintf(fph,"    <Animations count=\"%d\">\r\n", mesh->mAnimationCount );
      for (NxU32 i=0; i<mesh->mAnimationCount; i++)
      {
        print(fph,mesh->mAnimations[i]);
      }
      fi_fprintf(fph,"    </Animations>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output Materials
    //***
    //*******************************************************************
    if ( mesh->mMaterialCount )
    {
      fi_fprintf(fph,"    <Materials count=\"%d\">\r\n", mesh->mMaterialCount );
      for (NxU32 i=0; i<mesh->mMaterialCount; i++)
      {
        print(fph,mesh->mMaterials[i]);
      }
      fi_fprintf(fph,"    </Materials>\r\n", mesh->mMaterialCount );
    }


    //*******************************************************************
    //***
    //***  Output UserData
    //***
    //*******************************************************************
    // user data
    if ( mesh->mUserDataCount )
    {
      fi_fprintf(fph,"    <UserData count=\"%d\">\r\n", mesh->mUserDataCount );
      for (NxU32 i=0; i<mesh->mUserDataCount; i++)
      {
        print(fph,mesh->mUserData[i]);
      }
      fi_fprintf(fph,"    </UserData>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output UserBinaryData
    //***
    //*******************************************************************
    // user data
    if ( mesh->mUserBinaryDataCount )
    {
      fi_fprintf(fph,"    <UserBinaryData count=\"%d\">\r\n", mesh->mUserBinaryDataCount );
      for (NxU32 i=0; i<mesh->mUserBinaryDataCount; i++)
      {
        print(fph,mesh->mUserBinaryData[i]);
      }
      fi_fprintf(fph,"    </UserBinaryData>\r\n");
    }


    //*******************************************************************
    //***
    //***  Output Meshes
    //***
    //*******************************************************************
    if ( mesh->mMeshCount )
    {
      fi_fprintf(fph,"    <Meshes count=\"%d\">\r\n", mesh->mMeshCount );
      for (NxU32 i=0; i<mesh->mMeshCount; i++)
      {
        print(fph,mesh->mMeshes[i]);
      }
      fi_fprintf(fph,"    </Meshes>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output MeshInstances
    //***
    //*******************************************************************
    if ( mesh->mMeshInstanceCount )
    {
      fi_fprintf(fph,"    <MeshInstances count=\"%d\">\r\n", mesh->mMeshInstanceCount );
      for (NxU32 i=0; i<mesh->mMeshInstanceCount; i++)
      {
        print(fph,mesh->mMeshInstances[i]);
      }
      fi_fprintf(fph,"    </MeshInstances>\r\n");
    }

    //*******************************************************************
    //***
    //***  Output MeshCollisionRepresentations
    //***
    //*******************************************************************
    if ( mesh->mMeshCollisionCount )
    {
      fi_fprintf(fph,"    <MeshCollisionRepresentations count=\"%d\">\r\n", mesh->mMeshCollisionCount );
      for (NxU32 i=0; i<mesh->mMeshCollisionCount; i++)
      {
        print(fph,mesh->mMeshCollisionRepresentations[i]);
      }
      fi_fprintf(fph,"    </MeshCollisionRepresentations>\r\n");
    }

    fi_fprintf(fph,"  </MeshSystem>\r\n");
  }

  // ok..ready to serialize in the Ogre format..
  void serializeOgre(FILE_INTERFACE *fph,FILE_INTERFACE *exfph,MeshSystem *mesh,const char *saveName)
  {
    // ogre wants all the vertices in one big buffer..
    VertexPool< MeshVertex > bigPool;

    NxU32 vertexFlags = 0;
    for (NxU32 i=0; i<mesh->mMeshCount; i++)
    {
      Mesh *m = mesh->mMeshes[i];
      vertexFlags|=m->mVertexFlags;
      for (NxU32 k=0; k<m->mVertexCount; k++)
      {
        const MeshVertex &v = m->mVertices[k];
        bigPool.GetVertex(v);
      }
    }

    fi_fprintf(fph,"<mesh>\r\n");
    fi_fprintf(fph,"  <sharedgeometry vertexcount=\"%d\">\r\n", bigPool.GetSize() );

    fi_fprintf(fph,"    <vertexbuffer positions=\"%s\" normals=\"%s\">\r\n", (vertexFlags & MIVF_POSITION) ? "true" : "false", (vertexFlags & MIVF_NORMAL) ? "true" : "false" );
    NxI32 vcount = bigPool.GetSize();
    if ( vcount )
    {
      MeshVertex *data = bigPool.GetBuffer();
      for (NxI32 i=0; i<vcount; i++)
      {
        fi_fprintf(fph,"      <vertex>\r\n");
        fi_fprintf(fph,"        <position x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString(data->mPos[0]), FloatString(data->mPos[1]), FloatString(data->mPos[2]) );
        fi_fprintf(fph,"        <normal x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString(data->mNormal[0]), FloatString(data->mNormal[1]), FloatString(data->mNormal[2]) );
        fi_fprintf(fph,"      </vertex>\r\n");
        data++;
      }
    }
    fi_fprintf(fph,"    </vertexbuffer>\r\n");

    if ( vertexFlags & MIVF_COLOR )
    {
      fi_fprintf(fph,"    <vertexbuffer colours_diffuse=\"true\">\r\n");
      NxI32 vcount = bigPool.GetSize();
      MeshVertex *data = bigPool.GetBuffer();
      for (NxI32 i=0; i<vcount; i++)
      {
        fi_fprintf(fph,"      <vertex>\r\n");

        NxU32 a = data->mColor>>24;
        NxU32 r = (data->mColor>>16)&0xFF;
        NxU32 g = (data->mColor>>8)&0xFF;
        NxU32 b = (data->mColor&0xFF);
        NxF32 fa = (NxF32)a*(1.0f/255.0f);
        NxF32 fr = (NxF32)r*(1.0f/255.0f);
        NxF32 fg = (NxF32)g*(1.0f/255.0f);
        NxF32 fb = (NxF32)b*(1.0f/255.0f);
        fi_fprintf(fph,"        <colour_diffuse value=\"%s %s %s %s\" />\r\n",
          FloatString(fa),
          FloatString(fr),
          FloatString(fg),
          FloatString(fb) );

        fi_fprintf(fph,"      </vertex>\r\n");
        data++;
      }
      fi_fprintf(fph,"    </vertexbuffer>\r\n");
    }

    if ( vertexFlags & MIVF_TEXEL1 )
    {

      fi_fprintf(fph,"    <vertexbuffer texture_coord_dimensions_0=\"2\" texture_coords=\"1\">\r\n");
      NxI32 vcount = bigPool.GetSize();
      MeshVertex *data = bigPool.GetBuffer();
      for (NxI32 i=0; i<vcount; i++)
      {
        fi_fprintf(fph,"      <vertex>\r\n");
        fi_fprintf(fph,"        <texcoord u=\"%s\" v=\"%s\" />\r\n", FloatString(data->mTexel1[0]), FloatString(data->mTexel1[1]) );
        fi_fprintf(fph,"      </vertex>\r\n");
        data++;
      }
      fi_fprintf(fph,"    </vertexbuffer>\r\n");
    }

    fi_fprintf(fph,"   </sharedgeometry>\r\n");
    fi_fprintf(fph,"   <submeshes>\r\n");

    for (NxU32 i=0; i<mesh->mMeshCount; i++)
    {
      Mesh *m = mesh->mMeshes[i];
      for (NxU32 j=0; j<m->mSubMeshCount; j++)
      {
        SubMesh *sm = m->mSubMeshes[j];
        fi_fprintf(fph,"      <submesh material=\"%s\" usesharedvertices=\"true\" operationtype=\"triangle_list\">\r\n", sm->mMaterialName );
        fi_fprintf(fph,"        <faces count=\"%d\">\r\n", sm->mTriCount );
        for (NxU32 k=0; k<sm->mTriCount; k++)
        {
          NxU32 i1 = sm->mIndices[k*3+0];
          NxU32 i2 = sm->mIndices[k*3+1];
          NxU32 i3 = sm->mIndices[k*3+2];
          const MeshVertex &v1 = m->mVertices[i1];
          const MeshVertex &v2 = m->mVertices[i2];
          const MeshVertex &v3 = m->mVertices[i3];
          i1 = bigPool.GetVertex(v1);
          i2 = bigPool.GetVertex(v2);
          i3 = bigPool.GetVertex(v3);
          fi_fprintf(fph,"          <face v1=\"%d\" v2=\"%d\" v3=\"%d\" />\r\n", i1, i2, i3 );
        }
        fi_fprintf(fph,"       </faces>\r\n");
        fi_fprintf(fph,"       <boneassignments />\r\n");
        fi_fprintf(fph,"     </submesh>\r\n");
      }
    }
    fi_fprintf(fph,"  </submeshes>\r\n");
    if ( mesh->mSkeletonCount )
    {
      MeshSkeleton *sk = mesh->mSkeletons[0];
      if ( saveName )
      {
        const char *slash = lastSlash(saveName);
        if ( slash == 0 )
          slash = saveName;
        else
          slash++;

        char scratch[512];
        strcpy(scratch,slash);
        char *dot = stristr(scratch,".mesh.xml");
        if ( dot )
        {
          *dot = 0;
        }

        fi_fprintf(fph,"      <skeletonlink name=\"%s.skeleton\" />\r\n", scratch );
      }
      else
        fi_fprintf(fph,"      <skeletonlink name=\"%s\" />\r\n", sk->mName );
    }

    if ( vertexFlags & MIVF_BONE_WEIGHTING )
    {
      fi_fprintf(fph,"   <boneassignments>\r\n");
      NxI32 vcount = bigPool.GetSize();
      MeshVertex *data = bigPool.GetBuffer();
      for (NxI32 i=0; i<vcount; i++)
      {
        for (NxI32 j=0; j<4; j++)
        {
          if ( data->mWeight[j] == 0 ) break;
          fi_fprintf(fph,"       <vertexboneassignment vertexindex=\"%d\" boneindex=\"%d\" weight=\"%s\" />\r\n", i, data->mBone[j], FloatString(data->mWeight[j] ) );
        }
        data++;
      }
      fi_fprintf(fph,"   </boneassignments>\r\n");
    }
    fi_fprintf(fph,"</mesh>\r\n");

    // ok..now if we have a skeleton..
    fi_fprintf(exfph,"<skeleton>\r\n");
    if ( mesh->mSkeletonCount )
    {
      MeshSkeleton *skeleton = mesh->mSkeletons[0]; // only serialize one skeleton!
      fi_fprintf(exfph,"  <bones>\r\n");
      for (NxI32 i=0; i<skeleton->mBoneCount; i++)
      {
        MeshBone &b = skeleton->mBones[i];
        fi_fprintf(exfph,"    <bone id=\"%d\" name=\"%s\">\r\n", i, b.mName );
        fi_fprintf(exfph,"      <position x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString( b.mPosition[0] ), FloatString( b.mPosition[1] ), FloatString( b.mPosition[2] ) );
        NxF32 angle = 0;
        NxF32 axis[3] = { 0, 0, 0 };
        b.getAngleAxis(angle,axis);
        fi_fprintf(exfph,"      <rotation angle=\"%s\">\r\n", FloatString(angle) );
        fi_fprintf(exfph,"         <axis x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString( axis[0] ), FloatString( axis[1] ), FloatString( axis[2] ) );
        fi_fprintf(exfph,"      </rotation>\r\n");
        fi_fprintf(exfph,"      <scale x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString( b.mScale[0] ), FloatString( b.mScale[1] ), FloatString( b.mScale[2] ) );
        fi_fprintf(exfph,"    </bone>\r\n");
      }
      fi_fprintf(exfph,"  </bones>\r\n");
      fi_fprintf(exfph,"  <bonehierarchy>\r\n");
      for (NxI32 i=0; i<skeleton->mBoneCount; i++)
      {
        MeshBone &b = skeleton->mBones[i];
        if ( b.mParentIndex != -1 )
        {
          MeshBone &p = skeleton->mBones[b.mParentIndex];
          fi_fprintf(exfph,"    <boneparent bone=\"%s\" parent=\"%s\" />\r\n", b.mName, p.mName );
        }
      }
      fi_fprintf(exfph,"  </bonehierarchy>\r\n");
    }
    if ( mesh->mAnimationCount )
    {
      fi_fprintf(exfph,"   <animations>\r\n");

      for (NxI32 i=0; i<(NxI32)mesh->mAnimationCount; i++)
      {
        MeshAnimation *anim = mesh->mAnimations[i]; // only serialize one animation
        fi_fprintf(exfph,"   <animation name=\"%s\" length=\"%d\">\r\n", anim->mName, anim->mFrameCount );
        fi_fprintf(exfph,"     <tracks>\r\n");
        for (NxI32 j=0; j<anim->mTrackCount; j++)
        {
          MeshAnimTrack *track = anim->mTracks[j];

          fi_fprintf(exfph,"  <track bone=\"%s\">\r\n", track->mName );
          fi_fprintf(exfph,"     <keyframes>\r\n");

          NxF32 tm = 0;

          NxF32 base_inverse[16];
          fmi_identity(base_inverse);
          if ( mesh->mSkeletonCount )
          {
            MeshSkeleton *sk = mesh->mSkeletons[i];
            for (NxI32 i=0; i<sk->mBoneCount; i++)
            {
              MeshBone &b = sk->mBones[i];
              if ( strcmp(b.mName,track->mName) == 0 )
              {
                // ok..compose the local space transform..
                NxF32 local_matrix[16];
                fmi_composeTransform( b.mPosition, b.mOrientation, b.mScale, local_matrix );
                fmi_inverseTransform(local_matrix,base_inverse);
              }
            }
          }


          for (NxI32 k=0; k<track->mFrameCount; k++)
          {
            MeshAnimPose &p = track->mPose[k];

            NxF32 local_matrix[16];
            fmi_composeTransform(p.mPos,p.mQuat,p.mScale,local_matrix);
            fmi_multiply(local_matrix,base_inverse,local_matrix);

            NxF32 trans[3] = { 0, 0, 0 };
            NxF32 scale[3] = { 0, 0, 0 };
            NxF32 rot[4];

            fmi_decomposeTransform(local_matrix,trans,rot,scale);

            NxF32 angle = 0;
            NxF32 axis[3] = { 0, 0, 0 };

            fmi_getAngleAxis(angle,axis,rot);

            fi_fprintf(exfph,"  <keyframe time=\"%s\">\r\n", FloatString(tm) );
            fi_fprintf(exfph,"    <translate x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString(trans[0]), FloatString(trans[1]), FloatString(trans[2]) );
            fi_fprintf(exfph,"    <rotate angle=\"%s\">\r\n", FloatString(angle) );
            fi_fprintf(exfph,"      <axis x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString(axis[0]), FloatString(axis[1]), FloatString(axis[2]) );
            fi_fprintf(exfph,"    </rotate>\r\n");
            fi_fprintf(exfph,"    <scale x=\"%s\" y=\"%s\" z=\"%s\" />\r\n", FloatString( scale[0] ), FloatString(scale[1]), FloatString(scale[2]) );
            fi_fprintf(exfph,"  </keyframe>\r\n");

            tm+=track->mDtime;
          }
          fi_fprintf(exfph,"    </keyframes>\r\n");
          fi_fprintf(exfph,"  </track>\r\n");
        }
        fi_fprintf(exfph,"     </tracks>\r\n");
        fi_fprintf(exfph,"   </animation>\r\n");
      }

      fi_fprintf(exfph,"   </animations>\r\n");
    }
    fi_fprintf(exfph,"</skeleton>\r\n");
  }

  typedef std::vector< NxU32 > NxU32Vector;

  class Msave
  {
    public:
      const char *mMaterialName;
      NxU32Vector mIndices;
  };

  typedef std::vector< Msave > MsaveVector;

  void serializeWavefront(FILE_INTERFACE *fph,FILE_INTERFACE *exfph,MeshSystem *mesh,const char *saveName,const NxF32 *exportTransform)
  {
    fi_fprintf(fph,"# Asset '%s'\r\n", mesh->mAssetName );
    char scratch[512];
    strcpy(scratch,saveName);
    char *dot = (char *)lastDot(scratch);
    if ( dot )
    {
      *dot = 0;
    }
    strcat(scratch,".mtl");
    fi_fprintf(fph,"mtllib %s\r\n", scratch );

    for (NxU32 i=0; i<mesh->mMaterialCount; i++)
    {
      const MeshMaterial &m = mesh->mMaterials[i];
      fi_fprintf(exfph,"newmtl %s\r\n", m.mName );
      char scratch[512];
      strncpy(scratch,m.mName,512);
      char *plus = strstr(scratch,"+");
      if ( plus )
        *plus = 0;
      const char *diffuse = scratch;
      fi_fprintf(exfph,"map_Ka %s\r\n", diffuse );
    }

    if  ( mesh->mMeshInstanceCount )
    {

      MsaveVector meshes;
      VertexPool< MeshVertex > bigPool;

      for (NxU32 i=0; i<mesh->mMeshInstanceCount; i++)
      {
        MeshInstance &inst =  mesh->mMeshInstances[i];
        for (NxU32 j=0; j<mesh->mMeshCount; j++)
        {
          Mesh *m = mesh->mMeshes[j];
          if ( strcmp(m->mName,inst.mMeshName) == 0 )
          {
            NxF32 matrix[16];
            fmi_composeTransform(inst.mPosition,inst.mRotation,inst.mScale,matrix);
            NxF32 rotate[16];
            fmi_quatToMatrix(inst.mRotation,rotate);

            bool compute_normal = false;

            for (NxU32 k=0; k<m->mSubMeshCount; k++)
            {
              SubMesh *sm = m->mSubMeshes[k];
              Msave ms;
              ms.mMaterialName = sm->mMaterialName;
              for (NxU32 l=0; l<sm->mTriCount; l++)
              {
                NxU32 i1 = sm->mIndices[l*3+0];
                NxU32 i2 = sm->mIndices[l*3+1];
                NxU32 i3 = sm->mIndices[l*3+2];

                MeshVertex v1 = m->mVertices[i1];
                MeshVertex v2 = m->mVertices[i2];
                MeshVertex v3 = m->mVertices[i3];

                fmi_transform(matrix,v1.mPos,v1.mPos);
                fmi_transform(matrix,v2.mPos,v2.mPos);
                fmi_transform(matrix,v3.mPos,v3.mPos);
                fmi_transform(exportTransform,v1.mPos,v1.mPos);
                fmi_transform(exportTransform,v2.mPos,v2.mPos);
                fmi_transform(exportTransform,v3.mPos,v3.mPos);

                if ( l == 0 )
                {
                  if ( v1.mRadius == 1 || v2.mRadius == 1 || v3.mRadius == 1 )
                  {
                    compute_normal = true;
                  }
                  if ( v1.mNormal[0] == 0 && v1.mNormal[1] == 0 && v1.mNormal[2] == 0 ) compute_normal = true;
                }

                if ( compute_normal )
                {

                  v1.mRadius = 1;
                  v2.mRadius = 1;
                  v3.mRadius = 1;

                  NxF32 n[3];
                  fmi_computePlane(v3.mPos,v2.mPos,v1.mPos,n);

                  v1.mNormal[0]+=n[0];
                  v1.mNormal[1]+=n[1];
                  v1.mNormal[2]+=n[2];

                  v2.mNormal[0]+=n[0];
                  v2.mNormal[1]+=n[1];
                  v2.mNormal[2]+=n[2];

                  v3.mNormal[0]+=n[0];
                  v3.mNormal[1]+=n[1];
                  v3.mNormal[2]+=n[2];

                }
                else
                {

                  fmi_transform(rotate,v1.mNormal,v1.mNormal);
                  fmi_transform(rotate,v2.mNormal,v2.mNormal);
                  fmi_transform(rotate,v3.mNormal,v3.mNormal);

                  fmi_transformRotate(exportTransform,v1.mNormal,v1.mNormal);
                  fmi_transformRotate(exportTransform,v2.mNormal,v2.mNormal);
                  fmi_transformRotate(exportTransform,v3.mNormal,v3.mNormal);

                }

                i1 = bigPool.GetVertex(v1)+1;
                i2 = bigPool.GetVertex(v2)+1;
                i3 = bigPool.GetVertex(v3)+1;

                ms.mIndices.push_back(i1);
                ms.mIndices.push_back(i2);
                ms.mIndices.push_back(i3);

              }
              meshes.push_back(ms);
            }
            break;
          }
        }
      }

      NxI32 vcount = bigPool.GetVertexCount();

      if ( vcount )
      {
        MeshVertex *vb = bigPool.GetBuffer();
        for (NxI32 i=0; i<vcount; i++)
        {
          const MeshVertex &v = vb[i];
          fi_fprintf(fph,"v %s %s %s\r\n", FloatString(v.mPos[0]), FloatString(v.mPos[1]), FloatString(v.mPos[2]));
        }
        for (NxI32 i=0; i<vcount; i++)
        {
          const MeshVertex &v = vb[i];
          fi_fprintf(fph,"vt %s %s\r\n", FloatString(v.mTexel1[0]), FloatString(v.mTexel1[1]));
        }
        for (NxI32 i=0; i<vcount; i++)
        {
          MeshVertex &v = vb[i];
          fmi_normalize(v.mNormal);
          fi_fprintf(fph,"vn %s %s %s\r\n", FloatString(v.mNormal[0]), FloatString(v.mNormal[1]), FloatString(v.mNormal[2]));
        }
        MsaveVector::iterator i;
        for (i=meshes.begin(); i!=meshes.end(); ++i)
        {
          Msave &ms = (*i);
          fi_fprintf(fph,"usemtl %s\r\n", ms.mMaterialName );
          NxU32 tcount = (NxU32)ms.mIndices.size()/3;
          NxU32 *indices = &ms.mIndices[0];
          for (NxU32 k=0; k<tcount; k++)
          {
            NxU32 i1 = indices[k*3+0];
            NxU32 i2 = indices[k*3+1];
            NxU32 i3 = indices[k*3+2];
            fi_fprintf(fph,"f %d/%d/%d %d/%d/%d %d/%d/%d\r\n", i1, i1, i1, i2, i2, i2, i3, i3, i3 );
          }
        }
      }
    }
    else
    {
      MsaveVector meshes;
      VertexPool< MeshVertex > bigPool;

      for (NxU32 j=0; j<mesh->mMeshCount; j++)
      {
        Mesh *mm = mesh->mMeshes[j];
        bool compute_normal = false;
        for (NxU32 k=0; k<mm->mSubMeshCount; k++)
        {
          SubMesh *sm = mm->mSubMeshes[k];
          Msave ms;
          ms.mMaterialName = sm->mMaterialName;
          for (NxU32 l=0; l<sm->mTriCount; l++)
          {
            NxU32 i1 = sm->mIndices[l*3+0];
            NxU32 i2 = sm->mIndices[l*3+1];
            NxU32 i3 = sm->mIndices[l*3+2];

            MeshVertex v1 = mm->mVertices[i1];
            MeshVertex v2 = mm->mVertices[i2];
            MeshVertex v3 = mm->mVertices[i3];

            fmi_transform(exportTransform,v1.mPos,v1.mPos);
            fmi_transform(exportTransform,v2.mPos,v2.mPos);
            fmi_transform(exportTransform,v3.mPos,v3.mPos);

            if ( l == 0 )
            {
              if ( v1.mRadius == 1 || v2.mRadius == 1 || v3.mRadius == 1 )
              {
                compute_normal = true;
              }
              if ( v1.mNormal[0] == 0 && v1.mNormal[1] == 0 && v1.mNormal[2] == 0 ) compute_normal = true;
            }

            if ( compute_normal )
            {

              v1.mRadius = 1;
              v2.mRadius = 1;
              v3.mRadius = 1;

              NxF32 n[3];
              fmi_computePlane(v3.mPos,v2.mPos,v1.mPos,n);

              v1.mNormal[0]+=n[0];
              v1.mNormal[1]+=n[1];
              v1.mNormal[2]+=n[2];

              v2.mNormal[0]+=n[0];
              v2.mNormal[1]+=n[1];
              v2.mNormal[2]+=n[2];

              v3.mNormal[0]+=n[0];
              v3.mNormal[1]+=n[1];
              v3.mNormal[2]+=n[2];

            }
            else
            {
              fmi_transformRotate(exportTransform,v1.mNormal,v1.mNormal);
              fmi_transformRotate(exportTransform,v2.mNormal,v2.mNormal);
              fmi_transformRotate(exportTransform,v3.mNormal,v3.mNormal);
            }

            i1 = bigPool.GetVertex(v1)+1;
            i2 = bigPool.GetVertex(v2)+1;
            i3 = bigPool.GetVertex(v3)+1;

            ms.mIndices.push_back(i1);
            ms.mIndices.push_back(i2);
            ms.mIndices.push_back(i3);

          }
          meshes.push_back(ms);
        }
      }
      NxI32 vcount = bigPool.GetVertexCount();

      if ( vcount )
      {
        MeshVertex *vb = bigPool.GetBuffer();
        for (NxI32 i=0; i<vcount; i++)
        {
          const MeshVertex &v = vb[i];
          fi_fprintf(fph,"v %s %s %s\r\n", FloatString(v.mPos[0]), FloatString(v.mPos[1]), FloatString(v.mPos[2]));
        }
        for (NxI32 i=0; i<vcount; i++)
        {
          const MeshVertex &v = vb[i];
          fi_fprintf(fph,"vt %s %s\r\n", FloatString(v.mTexel1[0]), FloatString(v.mTexel1[1]));
        }
        for (NxI32 i=0; i<vcount; i++)
        {
          MeshVertex &v = vb[i];
          fmi_normalize(v.mNormal);
          fi_fprintf(fph,"vn %s %s %s\r\n", FloatString(v.mNormal[0]), FloatString(v.mNormal[1]), FloatString(v.mNormal[2]));
        }
        MsaveVector::iterator i;
        for (i=meshes.begin(); i!=meshes.end(); ++i)
        {
          Msave &ms = (*i);
          fi_fprintf(fph,"usemtl %s\r\n", ms.mMaterialName );
          NxU32 tcount = (NxU32)ms.mIndices.size()/3;
          NxU32 *indices = &ms.mIndices[0];
          for (NxU32 k=0; k<tcount; k++)
          {
            NxU32 i1 = indices[k*3+0];
            NxU32 i2 = indices[k*3+1];
            NxU32 i3 = indices[k*3+2];
            fi_fprintf(fph,"f %d/%d/%d %d/%d/%d %d/%d/%d\r\n", i1, i1, i1, i2, i2, i2, i3, i3, i3 );
          }
        }
      }
    }
  }

  virtual bool serializeMeshSystem(MeshSystem *mesh,MeshSerialize &data)
  {
    bool ret = false;

    FILE_INTERFACE *fph = fi_fopen("foo", "wmem", 0, 0);

    if ( fph )
    {
      if ( data.mFormat == MSF_OGRE3D )
      {
        FILE_INTERFACE *exfph = fi_fopen("foo", "wmem", 0, 0);
        serializeOgre(fph,exfph,mesh,data.mSaveFileName);
        size_t olen;
        void *temp = fi_getMemBuffer(exfph,&olen);
        if ( temp )
        {
          data.mExtendedData = (NxU8 *)MEMALLOC_MALLOC(olen);
          memcpy(data.mExtendedData,temp,olen);
          data.mExtendedLen = (NxU32)olen;
        }
        fi_fclose(exfph);
      }
      else if ( data.mFormat == MSF_EZMESH )
      {
        serializeEzm(fph,mesh);
      }
      else if ( data.mFormat == MSF_FBX )
      {
      	serializeFBX(fph,mesh);
      }
      else if ( data.mFormat == MSF_PSK )
      {
        FILE_INTERFACE *exfph = fi_fopen("foo", "wmem", 0, 0);
        serializePSK(fph,mesh,exfph);
        size_t olen;
        void *temp = fi_getMemBuffer(exfph,&olen);
        if ( temp )
        {
          data.mExtendedData = (NxU8 *)MEMALLOC_MALLOC(olen);
          memcpy(data.mExtendedData,temp,olen);
          data.mExtendedLen = (NxU32)olen;
        }
        fi_fclose(exfph);
      }
      else if ( data.mFormat == MSF_WAVEFRONT )
      {
        FILE_INTERFACE *exfph = fi_fopen("foo", "wmem", 0, 0);
        serializeWavefront(fph,exfph,mesh,data.mSaveFileName,data.mExportTransform);
        size_t olen;
        void *temp = fi_getMemBuffer(exfph,&olen);
        if ( temp )
        {
          data.mExtendedData = (NxU8 *)MEMALLOC_MALLOC(olen);
          memcpy(data.mExtendedData,temp,olen);
          data.mExtendedLen = (NxU32)olen;
        }
        fi_fclose(exfph);
      }
	  else if ( data.mFormat == MSF_ARM_XML || data.mFormat == MSF_ARM_BINARY )
	  {
		  MeshImporter *imp = locateMeshImporter("mesh.apx");
		  if ( imp )
		  {
			  NxU32 dlen;
			  const void *mem = imp->saveMeshSystem(mesh,dlen, data.mFormat == MSF_ARM_BINARY );
			  if( mem )
			  {
				  fi_fwrite(mem,dlen,1,fph);
				  imp->releaseSavedMeshSystem(mem);
			  }
		  }
	  }

      size_t olen;
      void *temp = fi_getMemBuffer(fph,&olen);
      if ( temp )
      {
        data.mBaseData = (NxU8 *)MEMALLOC_MALLOC(olen);
        memcpy(data.mBaseData,temp,olen);
        data.mBaseLen = (NxU32)olen;
        ret = true;
      }
      fi_fclose(fph);
    }

    return ret;
  }

  virtual  void             releaseSerializeMemory(MeshSerialize &data)
  {
    MEMALLOC_FREE(data.mBaseData);
    MEMALLOC_FREE(data.mExtendedData);
    data.mBaseData = 0;
    data.mBaseLen = 0;
    data.mExtendedData = 0;
    data.mExtendedLen = 0;
  }


  virtual const char   *    getFileRequestDialogString(void)
  {
    typedef std::vector< std::string > StringVector;
    StringVector descriptions;
    StringVector extensions;

    NxU32 count = getImporterCount();
    for (unsigned i=0; i<count; i++)
    {
      NVSHARE::MeshImporter *imp = getImporter(i);
      NxU32 ecount = imp->getExtensionCount();
      for (NxU32 j=0; j<ecount; j++)
      {
        const char *description = imp->getDescription(j);
        const char *itype = imp->getExtension(j);
        std::string desc = description;
        std::string ext = itype;
        descriptions.push_back(desc);
        extensions.push_back(ext);
      }
    }
    mFileRequest.clear();
    mFileRequest+="All (";
    count = (NxU32)descriptions.size();
    for (NxU32 i=0; i<count; i++)
    {
      mFileRequest+="*";
      mFileRequest+=extensions[i];
      if ( i != (count-1) )
        mFileRequest+=";";
    }
    mFileRequest+=")|";
    for (NxU32 i=0; i<count; i++)
    {
      mFileRequest+="*";
      mFileRequest+=extensions[i];
      if ( i != (count-1) )
        mFileRequest+=";";
    }
    mFileRequest+="|";
    for (NxU32 i=0; i<count; i++)
    {
      mFileRequest+=descriptions[i];
      mFileRequest+=" (*";
      mFileRequest+=extensions[i];
      mFileRequest+=")*";
      mFileRequest+="|*";
      mFileRequest+=extensions[i];
      if ( i != (count-1) )
        mFileRequest+="|";
    }
    return mFileRequest.c_str();
  }

  virtual void             setMeshImportApplicationResource(MeshImportApplicationResource *resource)
  {
    mApplicationResource = resource;
  }

  virtual MeshSkeletonInstance *createMeshSkeletonInstance(const MeshSkeleton &sk)
  {
    MeshSkeletonInstance *ret = 0;

    if ( sk.mBoneCount )
    {
      ret = MEMALLOC_NEW(MeshSkeletonInstance);
      ret->mName = sk.mName;
      ret->mBoneCount = sk.mBoneCount;
      ret->mBones = MEMALLOC_NEW(MeshBoneInstance)[sk.mBoneCount];
      for (NxI32 i=0; i<ret->mBoneCount; i++)
      {
        const MeshBone &src   = sk.mBones[i];
        MeshBoneInstance &dst = ret->mBones[i];
        dst.mBoneName = src.mName;
        dst.mParentIndex = src.mParentIndex;
        fmi_composeTransform(src.mPosition,src.mOrientation,src.mScale,dst.mLocalTransform);
	  }
      for (NxI32 i=0; i<ret->mBoneCount; i++)
      {
        const MeshBone &src   = sk.mBones[i];
		if (src.mParentIndex == -1)
			composeBoneTransform(ret, i);
	  }
    }
    return ret;
  }

  void composeBoneTransform(MeshSkeletonInstance* ret, NxI32 idx)
  {
	  MeshBoneInstance &bone = ret->mBones[idx];
	  if (bone.mParentIndex != -1)
	  {
		  MeshBoneInstance &parent = ret->mBones[bone.mParentIndex];
		  fmi_multiply(bone.mLocalTransform,parent.mTransform,bone.mTransform); // multiply times the parent matrix.
	  }
	  else
	  {
		  memcpy(bone.mTransform,bone.mLocalTransform,sizeof(NxF32)*16);
	  }
	  bone.composeInverse(); // compose the inverse transform.

      for (NxI32 i=0; i<ret->mBoneCount; i++)
      {
		  if (ret->mBones[i].mParentIndex == idx)
			  composeBoneTransform(ret, i);
	  }
  }

  virtual  void  releaseMeshSkeletonInstance(MeshSkeletonInstance *sk)
  {
    if ( sk )
    {
      delete []sk->mBones;
      delete sk;
    }
  }

  virtual bool  sampleAnimationTrack(NxI32 trackIndex,const MeshSystem *msystem,MeshSkeletonInstance *skeleton)
  {
    bool ret = false;

    if ( msystem && skeleton && msystem->mAnimationCount )
    {
      MeshAnimation *anim = msystem->mAnimations[0]; // got the animation.
      for (NxI32 i=0; i<skeleton->mBoneCount; i++)
      {
        MeshBoneInstance &b = skeleton->mBones[i];
        // ok..look for this track in the animation...
        NxF32 transform[16];

        MeshAnimTrack *track = 0;
        for (NxI32 j=0; j<anim->mTrackCount; j++)
        {
          MeshAnimTrack *t = anim->mTracks[j];
          if ( strcmp(t->mName,b.mBoneName) == 0 ) // if the names match
          {
            track = t;
            break;
          }
        }
        if ( track && track->mFrameCount )
        {
          NxI32 tindex = trackIndex% track->mFrameCount;
          MeshAnimPose &p = track->mPose[tindex];
          fmi_composeTransform(p.mPos,p.mQuat,p.mScale,transform);
        }
        else
        {
          memcpy(transform,b.mLocalTransform,sizeof(NxF32)*16);
        }
#if 0
        if ( b.mParentIndex != -1 )
        {
          MeshBoneInstance &parent = skeleton->mBones[b.mParentIndex];
          fmi_multiply(transform,parent.mAnimTransform,b.mAnimTransform); // multiply times the parent matrix.
        }
        else
        {
          memcpy(b.mAnimTransform,transform,sizeof(NxF32)*16);
//          fmi_identity(b.mAnimTransform);
        }
        fmi_multiply(b.mInverseTransform,b.mAnimTransform,b.mCompositeAnimTransform);
#else
		memcpy(b.mAnimTransform,transform,sizeof(NxF32)*16);
	  }
      for (NxI32 i=0; i<skeleton->mBoneCount; i++)
	  {
		  if ( skeleton->mBones[i].mParentIndex == -1 )
			  poseBone( skeleton, i );
#endif
      }
      ret = true;
    }

    return ret;
  }

  void poseBone(MeshSkeletonInstance *skeleton, NxI32 boneIdx)
  {
	  MeshBoneInstance &b = skeleton->mBones[boneIdx];
	  if ( b.mParentIndex != -1 )
	  {
		  MeshBoneInstance &parent = skeleton->mBones[b.mParentIndex];
		  fmi_multiply(b.mAnimTransform,parent.mAnimTransform,b.mAnimTransform); // multiply times the parent matrix.
	  }
	  fmi_multiply(b.mInverseTransform,b.mAnimTransform,b.mCompositeAnimTransform);

	  // recurse to children.
	  for ( NxI32 i = 0; i < skeleton->mBoneCount; ++i )
	  {
          if ( skeleton->mBones[i].mParentIndex == boneIdx )
			  poseBone( skeleton, i );
	  }
  }

  void transformPoint(const NxF32 v[3],NxF32 t[3],const NxF32 matrix[16])
  {
    NxF32 tx = (matrix[0*4+0] * v[0]) +  (matrix[1*4+0] * v[1]) + (matrix[2*4+0] * v[2]) + matrix[3*4+0];
    NxF32 ty = (matrix[0*4+1] * v[0]) +  (matrix[1*4+1] * v[1]) + (matrix[2*4+1] * v[2]) + matrix[3*4+1];
    NxF32 tz = (matrix[0*4+2] * v[0]) +  (matrix[1*4+2] * v[1]) + (matrix[2*4+2] * v[2]) + matrix[3*4+2];
    t[0] = tx;
    t[1] = ty;
    t[2] = tz;
  }

  void rotatePoint(const NxF32 v[3],NxF32 t[3],const NxF32 matrix[16])
  {
	  NxF32 tx = (matrix[0*4+0] * v[0]) +  (matrix[1*4+0] * v[1]) + (matrix[2*4+0] * v[2]);
	  NxF32 ty = (matrix[0*4+1] * v[0]) +  (matrix[1*4+1] * v[1]) + (matrix[2*4+1] * v[2]);
	  NxF32 tz = (matrix[0*4+2] * v[0]) +  (matrix[1*4+2] * v[1]) + (matrix[2*4+2] * v[2]);
	  t[0] = tx;
	  t[1] = ty;
	  t[2] = tz;
  }


  void transformVertex(const MeshVertex &src,MeshVertex &dst,MeshSkeletonInstance *skeleton)
  {
    NxI32 bone = src.mBone[0];
    NxF32 weight = src.mWeight[0];
    assert (bone >= 0 && bone < skeleton->mBoneCount );
	memcpy(&dst,&src,sizeof(MeshVertex));
    if ( weight > 0 && bone >= 0 && bone < skeleton->mBoneCount )
    {
		{
		  NxF32 result[3];
		  dst.mPos[0] = 0;
		  dst.mPos[1] = 0;
		  dst.mPos[2] = 0;
		  for (NxI32 i=0; i<4; i++)
		  {
			bone = src.mBone[i];
			weight = src.mWeight[i];
			if ( weight == 0 )
			  break;
			transformPoint(src.mPos,result,skeleton->mBones[bone].mCompositeAnimTransform);
			dst.mPos[0]+=result[0]*weight;
			dst.mPos[1]+=result[1]*weight;
			dst.mPos[2]+=result[2]*weight;
		  }
		}
#if 0
		{
			NxF32 result[3];
			dst.mNormal[0] = 0;
			dst.mNormal[1] = 0;
			dst.mNormal[2] = 0;
			for (NxI32 i=0; i<4; i++)
			{
				bone = src.mBone[i];
				weight = src.mWeight[i];
				if ( weight == 0 )
					break;
				rotatePoint(src.mPos,result,skeleton->mBones[bone].mCompositeAnimTransform);
				dst.mNormal[0]+=result[0]*weight;
				dst.mNormal[1]+=result[1]*weight;
				dst.mNormal[2]+=result[2]*weight;
			}
		}
#endif
    }
  }

  virtual void transformVertices(NxU32 vcount,
                                 const MeshVertex *source_vertices,
                                 MeshVertex *dest_vertices,
                                 MeshSkeletonInstance *skeleton)
  {
    for (NxU32 i=0; i<vcount; i++)
    {
      transformVertex(*source_vertices,*dest_vertices,skeleton);
      source_vertices++;
      dest_vertices++;
    }
  }

  virtual void rotate(MeshSystemContainer *msc,NxF32 rotX,NxF32 rotY,NxF32 rotZ)  // rotate mesh system using these euler angles expressed as degrees.
  {
    MeshBuilder *b = (MeshBuilder *)msc;
    if ( b )
    {
      b->rotate(rotX,rotY,rotZ);
    }
  }

  virtual void scale(MeshSystemContainer *msc,NxF32 s)
  {
    MeshBuilder *b = (MeshBuilder *)msc;
    if ( b )
    {
      b->scale(s);
    }
  }

  virtual MeshImportInterface * getMeshImportInterface(MeshSystemContainer *msc)
  {
    MeshImportInterface *ret = 0;

    if ( msc )
    {
      MeshBuilder *b = (MeshBuilder *)msc;
      ret = static_cast< MeshImportInterface * >(b);
    }
    return ret;
  }

  virtual void gather(MeshSystemContainer *msc)
  {
    if ( msc )
    {
      MeshBuilder *b = (MeshBuilder *)msc;
      b->gather();
    }
  }

private:
  std::string         mCtype;
  std::string         mSemantic;
  std::string         mFileRequest;
  MeshImporterVector  mImporters;
  MeshImportApplicationResource *mApplicationResource;
  KeyValueIni        *mINI;
};

};  // End of Namespace


using namespace NVSHARE;


static MyMeshImport *gInterface=0;

extern "C"
{
#ifdef PLUGINS_EMBEDDED
  MeshImport * getInterfaceMeshImport(NxI32 version_number)
#else
  MESHIMPORT_API MeshImport * getInterface(NxI32 version_number)
#endif
{
  assert( gInterface == 0 );
  if ( gInterface == 0 && version_number == MESHIMPORT_VERSION )
  {
    gInterface = MEMALLOC_NEW(MyMeshImport);
  }
  return static_cast<MeshImport *>(gInterface);
};

};  // End of namespace PATHPLANNING

using namespace NVSHARE;

#ifndef PLUGINS_EMBEDDED

using namespace NVSHARE;

#ifdef WIN32

#include <windows.h>

BOOL APIENTRY DllMain( HANDLE ,
                       DWORD  ul_reason_for_call,
                       LPVOID )
{
  NxI32 ret = 0;

  switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
      ret = 1;
			break;
		case DLL_THREAD_ATTACH:
      ret = 2;
			break;
		case DLL_THREAD_DETACH:
      ret = 3;
			break;
		case DLL_PROCESS_DETACH:
			break;
    }
    return TRUE;
}

#endif

#endif
