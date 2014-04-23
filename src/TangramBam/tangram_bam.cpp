#include "tangram_bam.h"
#include <stdio.h>
#include <limits.h>
#include <getopt.h>

extern "C" {
#include "../OutSources/samtools/bam.h"
#include "SR_HashRegionTable.h"
#include "SR_QueryRegion.h"
}

#include <vector>
#include <string>
#include <iostream>

#include "api/BamReader.h"
#include "api/BamWriter.h"
#include "../OutSources/fasta/Fasta.h"
#include "../OutSources/stripedSW/ssw_cpp.h"
#include "special_hasher.h"
#include "hashes_collection.h"

using namespace std;

namespace {
StripedSmithWaterman::Aligner aligner_;
const StripedSmithWaterman::Filter kFilter(true, false, 0, 32467);
const int kAlignmentMapSize = 10000;
const float kSoftClipRate = 0.15; // the max ratio of allowed soft clips
const int kRequestedBases = 20;
int kRequiredMatch;
}

void ShowHelp() {
  fprintf(stderr, "\n");
  fprintf(stderr, "Usage: tangram_bam [options] -i <in_bam> -r <ref_fa> -o <out_bam>\n\n");

  fprintf(stderr, "\nMandatory arguments:\n");
  fprintf(stderr, "                     -i --input FILE   The input of bam file [stdin].\n");
  fprintf(stderr, "                     -r --ref FILE     The input of special reference file.\n");
  fprintf(stderr, "                     -o --output FILE  The output of bam file [stdout].\n");

  fprintf(stderr, "\nOptions:\n");
  fprintf(stderr, "                     -h --help                    Print this help message.\n");
  fprintf(stderr, "                     -t --target-ref-name STRING  Chromosome region.\n");
  fprintf(stderr, "                     -m --required-match INT      The number of required matches.\n");
  fprintf(stderr, "                                                  between reads and special references [50].\n");

  fprintf(stderr, "\nNotes:\n");
  fprintf(stderr, "       1. tangram_bam will add ZA tags that are required for the following detection.\n");

}

bool OpenBams(
    const string& infilename,
    const string& outfilename,
    const string& command_line,
    BamTools::BamReader* reader,
    BamTools::BamWriter* writer) {
  
  reader->Open(infilename);
  if (!reader->IsOpen()) {
    fprintf(stderr, "ERROR: The bam file, %s, cannot be open\n", infilename.c_str());
    return false;
  }

  // Load header and ref
  string header = reader->GetHeaderText();
  size_t pos1 = header.find("SO:");
  if (pos1 != string::npos) {
    size_t pos2 = header.find("SO:queryname");
    if (pos2 != string::npos) header.replace(pos2, 12, "SO:unsorted");
    pos2 = header.find("SO:coordinate");
    if (pos2 != string::npos) header.replace(pos2, 13, "SO:unsorted");
  }
  header += "@PG\tID:tangram_bam\tCL:";
  header += (command_line + '\n');
  BamTools::RefVector ref = reader->GetReferenceData();

  if (!writer->Open(outfilename, header, ref)) {
    fprintf(stderr, "ERROR: The bam file, %s, cannot be open\n", outfilename.c_str());
    reader->Close();
    return false;
  }

  return true;
}

inline bool LoadReference(const char* fa, FastaReference* fasta) {
  string filename = fa;
  fasta->open(filename);

  return true;
}

bool ConcatenateSpecialReference(
    FastaReference* fasta, 
    SpecialReference* s_ref) {
  int total_len = 0;
  for (vector<string>::const_iterator ite = fasta->index->sequenceNames.begin();
       ite != fasta->index->sequenceNames.end(); ++ite) {
    s_ref->concatnated += fasta->getSequence(*ite).c_str();
    s_ref->ref_lens.push_back(fasta->sequenceLength(*ite));
    s_ref->ref_names.push_back(*ite);
    total_len += fasta->sequenceLength(*ite);
  }

  s_ref->concatnated_len = total_len;

  return true;
}

void GetReverseComplement(const string& query, string* reverse) {
  reverse->clear();

  for (string::const_reverse_iterator ite = query.rbegin(); ite != query.rend(); ++ite) {
    switch(*ite) {
      case 'A': *reverse += 'T'; break;
      case 'C': *reverse += 'G'; break;
      case 'G': *reverse += 'C'; break;
      case 'T': *reverse += 'A'; break;
      default: *reverse += 'N';
    }
  }
}

/*
void Align(
    const string& query,
    const StripedSmithWaterman::Aligner& aligner,
    StripedSmithWaterman::Alignment* alignment) {
    
    alignment->Clear();
    aligner.Align(query.c_str(), kFilter, alignment);
}
*/

void GetZa(const Alignment& al, const Alignment& mate, string* za) {
  const bool mate1 = al.bam_alignment.IsFirstMate();
  Alignment const *mate1_ptr, *mate2_ptr;

  if (mate1) {
    *za = "<@;";
    mate1_ptr = &al;
    mate2_ptr = &mate;
  } else {
    *za = "<&;";
    mate1_ptr = &mate;
    mate2_ptr = &al;
  }

  *za += (std::to_string(static_cast<long long>(mate1_ptr->bam_alignment.MapQuality)) + ";;" 
          + (mate1_ptr->hit_insertion ? mate1_ptr->ins_prefix : "") 
	  + ";1;");
  
  if (mate1) {
    *za += ";><&;";
  } else {
    *za += ";><@;";
  }

  *za += (std::to_string(static_cast<long long>(mate2_ptr->bam_alignment.MapQuality)) + ";;" 
          + (mate2_ptr->hit_insertion ? mate2_ptr->ins_prefix : "") 
	  + ";1;;>");

}

void WriteAlignment(
    const Alignment& mate,
    Alignment* al,
    BamTools::BamWriter* writer) {
  
  string za;
  GetZa(*al, mate, &za);
  al->bam_alignment.AddTag("ZA","Z",za);

  writer->SaveAlignment(al->bam_alignment);
}

void WriteAlignment(
    Alignment* al,
    BamTools::BamWriter* writer) {
  
    string za;
    if (al->bam_alignment.IsPaired()) { // paired-end read
      if (al->bam_alignment.IsFirstMate()) {
        za = "<@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
           + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>"
           + "<&;0;;;0;;>";
      } else {
        za = "<&;0;;;0;;><@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
             + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>";
      }
    } else { // sinle-end read
      za = "<@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
           + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>";
    }

    al->bam_alignment.AddTag("ZA","Z",za);
    writer->SaveAlignment(al->bam_alignment);
}

void WriteAlignment(map<string, Alignment>* al_map_ite, 
                    BamTools::BamWriter* writer) {
		    
  for (map<string, Alignment>::iterator ite = al_map_ite->begin();
       ite != al_map_ite->end(); ++ite) {
    Alignment* al = &(ite->second);
    string za;
    if (al->bam_alignment.IsPaired()) { // paired-end read
      if (al->bam_alignment.IsFirstMate()) {
        za = "<@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
           + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>"
           + "<&;0;;;0;;>";
      } else {
        za = "<&;0;;;0;;><@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
             + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>";
      }
    } else { // sinle-end read
      za = "<@;" + std::to_string(static_cast<long long>(al->bam_alignment.MapQuality)) + ";;"
           + (al->hit_insertion ? al->ins_prefix : "") + ";1;;>";
    }

    al->bam_alignment.AddTag("ZA","Z",za);

    writer->SaveAlignment(al->bam_alignment);
  }
}

// Return true if an alignment contains too many soft clips
inline bool IsTooManyClips (const BamTools::BamAlignment& al, int* clip) {
  if (al.CigarData.empty()) return true;

  *clip = 0;
  if ((al.CigarData.begin())->Type == 'S') *clip += (al.CigarData.begin())->Length;
  if ((al.CigarData.rbegin())->Type == 'S') *clip += (al.CigarData.rbegin())->Length;

  if (*clip > (al.Length * kSoftClipRate)) return true;
  else return false;
}

inline void MarkAsUnmapped (BamTools::BamAlignment* al, BamTools::BamAlignment* mate) {
  int al_clip = 0, mate_clip =0;
  const bool al_unmapped   = IsTooManyClips(*al, &al_clip);
  const bool mate_unmapped = IsTooManyClips(*mate, &mate_clip);

  if (!al_unmapped && !mate_unmapped) {
    // nothing
  } else {
    if (al_unmapped && mate_unmapped) { // both mates are unmapped
      if (al_clip > mate_clip) {
        // pick the mate with more clips to be unmapped
	// that is al
        al->SetIsMapped(false);
        mate->SetIsMapped(true);
        al->SetIsMateMapped(true);
        mate->SetIsMateMapped(false);
      } else {
        // pick the mate with more clips to be unmapped
	// that is mate
        al->SetIsMapped(true);
        mate->SetIsMapped(false);
        al->SetIsMateMapped(false);
        mate->SetIsMateMapped(true);
      }
    } else { // not both mates are unmapped
      al->SetIsMapped(!al_unmapped);
      mate->SetIsMapped(!mate_unmapped);
      al->SetIsMateMapped(!mate_unmapped);
      mate->SetIsMateMapped(!al_unmapped);
    }
  }
}

void StoreAlignment(
    Alignment* al,
    vector<map<string, Alignment> > *al_maps,
    BamTools::BamWriter* writer) {
  int ref_id = al->bam_alignment.MateRefID;
  map<string, Alignment>* al_map = &((*al_maps)[ref_id]);
  map<string, Alignment>::iterator ite = al_map->find(al->bam_alignment.Name);
  if (ite == al_map->end()) { // cannot find the mate in the map
    WriteAlignment(al, writer);
  } else {
    WriteAlignment(ite->second, al, writer);
    al_map->erase(ite);
  }
}

void StoreAlignment(
    Alignment* al,
    map<string, Alignment>*& al_map_cur,
    map<string, Alignment>*& al_map_pre,
    BamTools::BamWriter* writer) {
  // Clear up the buffers once the al_map_cur buffer is full
  // 1. Clear up al_map_pre
  // 2. move al_map_cur to al_map_pre
  if ((static_cast<int>(al_map_cur->size()) > kAlignmentMapSize)) {
    WriteAlignment(al_map_pre, writer);
    al_map_pre->clear();
    map<string, Alignment> *tmp = al_map_pre;
    al_map_pre = al_map_cur;
    al_map_cur = tmp;
  }

  map<string, Alignment>::iterator ite_cur = al_map_cur->find(al->bam_alignment.Name);
  if (ite_cur == al_map_cur->end()) {
    if (al_map_pre != NULL) {
      map<string, Alignment>::iterator ite_pre = al_map_pre->find(al->bam_alignment.Name);
      if (ite_pre == al_map_pre->end()) { // al is not found in cur or pre either
        (*al_map_cur)[al->bam_alignment.Name] = *al;
      } else { // find the mate in al_map_pre
        if (al->bam_alignment.IsMapped() && ite_pre->second.bam_alignment.IsMapped())
          MarkAsUnmapped(&(al->bam_alignment), &(ite_pre->second.bam_alignment));
	WriteAlignment(ite_pre->second, al, writer);
        WriteAlignment(*al, &(ite_pre->second), writer);
        al_map_pre->erase(ite_pre);
      }
    } else { // al is not found in cur and pre is NULL
      (*al_map_cur)[al->bam_alignment.Name] = *al;
    }
  } else { // find the mate in al_map_cur
    if (al->bam_alignment.IsMapped() && ite_cur->second.bam_alignment.IsMapped())
      MarkAsUnmapped(&(al->bam_alignment), &(ite_cur->second.bam_alignment));
    WriteAlignment(ite_cur->second, al, writer);
    WriteAlignment(*al, &(ite_cur->second), writer);
    al_map_cur->erase(ite_cur);
  }
    
}

bool ParseArguments(const int argc, char* const * argv, Param* param) {
  if (argc == 1) { // no argument
    ShowHelp();
    return false;
  }

  // record command line
  param->command_line = argv[0];
  for ( int i = 1; i < argc; ++i ) {
    param->command_line += " ";
    param->command_line += argv[i];
  }

  const char *short_option = "hi:o:r:t:";
  const struct option long_option[] = {
    {"help", no_argument, NULL, 'h'},
    {"input", required_argument, NULL, 'i'},
    {"output", required_argument, NULL, 'o'},
    {"ref", required_argument, NULL, 'r'},
    {"target-ref-name", required_argument, NULL, 't'},
    {"required-match", required_argument, NULL, 'm'},

    {0, 0, 0, 0}
  };

  int c = 0;
  bool show_help = false;
  while (true) {
    int option_index = 0;
    c = getopt_long(argc, argv, short_option, long_option, &option_index);

    if (c == -1) // end of options
      break;

    switch (c) {
      case 'h': show_help = true; break;
      case 'i': param->in_bam = optarg; break;
      case 'o': param->out_bam = optarg; break;
      case 'r': param->ref_fasta = optarg; break;
      case 't': param->target_ref_name = optarg; break;
      case 'm': param->required_match = atoi(optarg); break;
    }
  }

  if (show_help || param->ref_fasta.empty() || (param->required_match <= 0)) {
    ShowHelp();
    return false;
  }

  kRequiredMatch = param->required_match;

  return true;
}
/*
int PickBestAlignment(const int& request_score, 
                      const StripedSmithWaterman::Alignment& alignment, 
		      const SpecialReference& s_ref) {
#ifdef TB_VERBOSE_DEBUG
  fprintf(stderr, "SSW score: %d, request_score: %d\n", alignment.sw_score, request_score);
  fprintf(stderr, "SSW ref_begin: %d, ref_end: %d\n", alignment.ref_begin, alignment.ref_end);
  fprintf(stderr, "cigar: %s\n", alignment.cigar_string.c_str());
#endif

  if (alignment.sw_score < (request_score * 1.4)) {
    return -1; // no proper alignment is found
  } else {
    int accu_len = 0;
#ifdef TB_VERBOSE_DEBUG
      fprintf(stderr, "accu_len:");
#endif
    for (unsigned int i = 0; i < s_ref.ref_lens.size(); ++i) {
      accu_len += s_ref.ref_lens[i];
#ifdef TB_VERBOSE_DEBUG
      fprintf(stderr, "\t%d: %d\n", i, accu_len);
#endif
      if (alignment.ref_begin < accu_len) {
        if (alignment.ref_end < accu_len) return i;
	else return -1;
      }
    }
  }

  return -1;
}
*/
inline bool IsProblematicAlignment(const BamTools::BamAlignment& al) {
  if (!al.IsMapped()) return true;
  if (al.RefID != al.MateRefID) return true;
  if (al.CigarData.size() > 5) return true;
  int clip = 0;
  if (IsTooManyClips(al, &clip)) return true;

  return false;
}

inline void StoreInBuffer(
    Alignment* al,
    vector<map<string, Alignment> >* al_maps) {
  int ref_id = al->bam_alignment.RefID;
  ((*al_maps)[ref_id])[al->bam_alignment.Name] = *al;
}

bool ConvertBamAlignmentToQueryRegion(
    const string& bases,
    SR_QueryRegion* qr) {
  qr->orphanSeq = (char*)malloc(bases.size() + 1); // free in LoadHash
  memcpy(qr->orphanSeq, bases.c_str(), bases.size());
  qr->orphanSeq[bases.size()] = '\0';
  qr->pOrphan = bam_init1(); // free in LoadHash
  qr->pOrphan->core.l_qseq  = bases.size();

  return true;
}

bool LoadHash(
    const string& bases,
    const SR_Reference* ref,
    const SR_InHashTable* hash_table,
    HashRegionTable* hashes,
    Scissors::HashesCollection* hashes_collection) {

  SR_QueryRegion* query_region = SR_QueryRegionAlloc();
  ConvertBamAlignmentToQueryRegion(bases, query_region);

  HashRegionTableInit(hashes, bases.size());
  SR_QueryRegionSetRangeSpecial(query_region, ref->seqLen);
  HashRegionTableLoad(hashes, hash_table, query_region);
  hashes_collection->Init(*(hashes->pBestCloseRegions));
  hashes_collection->SortByLength();

  free(query_region->orphanSeq);
  bam_destroy1(query_region->pOrphan);
  SR_QueryRegionFree(query_region);

  if ((hashes_collection->Get(hashes_collection->GetSize() - 1) == NULL))
    return false;
  if ((hashes_collection->Get(hashes_collection->GetSize() - 1)->length == 0))
    return false;

  return true;
}

int GetHashId(
    const BestRegion& region,
    const unsigned int& required_length,
    const SR_RefHeader* reference_header,
    const SR_Reference* reference_special,
    SR_RefView** special_ref_view,
    uint32_t* pos) {
  if (region.length < required_length) return -1;

  int32_t ref_id = 0;
  //uint32_t pos = 0;
  *pos = 0;

  //SR_RefView* special_ref_view = SR_RefViewAlloc();
  const SR_Status ok = 
    SR_GetRefFromSpecialPos(*special_ref_view, &ref_id, pos, reference_header, reference_special, region.refBegins[0]);

  //SR_RefViewFree(special_ref_view);

  if (ok != SR_OK) return -1;
  else return ref_id;
}

int GetAlignment(
    //const BamTools::BamAlignment& bam_alignment,
    const string& bases,
    const int& length,
    const BestRegion& region,
    const SR_RefHeader* reference_header,
    const SR_Reference* reference_special) {
  
  uint32_t pos = 0;
  SR_RefView* special_ref_view = SR_RefViewAlloc();
  const int special_ref_id = GetHashId(region, kRequestedBases, reference_header, reference_special, &special_ref_view, &pos);

  if (special_ref_id == -1) {
    SR_RefViewFree(special_ref_view);
    return -1;
  }

  int forward_shift = 0;
  if (static_cast <int> (pos) < length) forward_shift = pos;
  else forward_shift = length;

  int backward_shift = 0;
  if ((pos + length + 1) > special_ref_view->seqLen) backward_shift = special_ref_view->seqLen - pos - 1;
  else backward_shift = length;

  int begin = region.refBegins[0] - forward_shift;
  int end   = region.refBegins[0] + backward_shift;

  StripedSmithWaterman::Alignment alignment;
  const char* ref = reference_special->sequence + begin;
  const int ref_length = end - begin + 1;
  for(int i= begin; i< end; ++i) {
  }
  aligner_.Align(bases.c_str(), ref, ref_length, kFilter, &alignment);

  //int reauired_score = (bam_alignment.Length > 100) ? 140 : (bam_alignment.Length * 1.4);   
  int reauired_score = kRequiredMatch * 2; // 2 is the match score
  if (alignment.sw_score < reauired_score) {
    SR_RefViewFree(special_ref_view);
    return -1;
  } else {
    SR_RefViewFree(special_ref_view);
    return special_ref_id;
  }
}

void LoadAlignmentsNotInTargetChr(
    const int& target_ref_id,
    const SR_Reference* reference,
    const SR_InHashTable* hash_table,
    const SR_RefHeader* reference_header,
    const SpecialReference& s_ref,
    BamTools::BamReader* reader,
    vector<map<string, Alignment> >* al_maps) {
  
  BamTools::BamRegion region1, region2;
  bool has_region1 = false, has_region2 = false;

  const BamTools::RefVector& references = reader->GetReferenceData();

  // the target region is not the first chromosome
  if (target_ref_id != 0) {
    const BamTools::RefData& ref_data = references.at(target_ref_id - 1);
    region1.LeftRefID     = 0;
    region1.LeftPosition  = 0;
    region1.RightRefID    = target_ref_id - 1;
    region1.RightPosition = ref_data.RefLength;
    has_region1 = true;
  }

  // the target region is not the last chromosome
  if (target_ref_id != reader->GetReferenceCount() - 1) {
    const BamTools::RefData& ref_data = 
          references.at(reader->GetReferenceCount() - 1);
    region2.LeftRefID     = target_ref_id + 1;
    region2.LeftPosition  = 0;
    region2.RightRefID    = reader->GetReferenceCount() - 1;
    region2.RightPosition = ref_data.RefLength;
    has_region2 = true;
  }

  BamTools::BamAlignment bam_alignment;
  StripedSmithWaterman::Alignment alignment;
  Alignment al;

  // Load alignments in region1
  if (has_region1 && reader->SetRegion(region1)) {
    HashRegionTable* hashes = HashRegionTableAlloc();
    while (reader->GetNextAlignment(bam_alignment)) {
      int index = -1;
      if (bam_alignment.MateRefID == target_ref_id) {
        Scissors::HashesCollection hashes_collection;
        const bool get_hash = 
          LoadHash(bam_alignment.QueryBases, reference, hash_table, hashes, &hashes_collection);
        if (get_hash) {
          const int id = hashes_collection.GetSize() - 1;
          //index = GetHashId(*(hashes_collection.Get(id)), kRequestedBases, reference_header, reference);
          index = GetAlignment(bam_alignment.QueryBases, bam_alignment.QueryBases.size(), *(hashes_collection.Get(id)), reference_header, reference);
        }
        //Align(bam_alignment.QueryBases, aligner, &alignment);
        //index = PickBestAlignment(bam_alignment.Length, alignment, s_ref);
        if (index == -1) { // try the reverse complement sequences
          string reverse;
          GetReverseComplement(bam_alignment.QueryBases, &reverse);
	  //bam_alignment.QueryBases = reverse;
          #ifdef TB_VERBOSE_DEBUG
          fprintf(stderr, "%s\n", reverse.c_str());
          #endif
          Scissors::HashesCollection hashes_collection2;
          const bool get_hash = 
            LoadHash(reverse, reference, hash_table, hashes, &hashes_collection2);
          if (get_hash) {
            const int id = hashes_collection2.GetSize() - 1;
            //index = GetHashId(*(hashes_collection2.Get(id)), kRequestedBases, reference_header, reference);
            index = GetAlignment(reverse, reverse.size(), *(hashes_collection2.Get(id)), reference_header, reference);
          }
          //Align(reverse, aligner, &alignment);
          //index = PickBestAlignment(bam_alignment.Length, alignment, s_ref);
        } // end if (index == -1)
      
      al.Clear();
      al.bam_alignment = bam_alignment;
      al.hit_insertion = (index == -1) ? false: true;
      al.ins_prefix    = (index == -1) ? "" : s_ref.ref_names[index].substr(8,2);
      StoreInBuffer(&al, al_maps);

      #ifdef TB_VERBOSE_DEBUG
      fprintf(stderr, "SP mapped: %c\tSP:%s\n", (index == -1) ? 'F' : 'T', al.ins_prefix.c_str());
      #endif
      } // end of
    }
    HashRegionTableFree(hashes);
  }

  // Load alignments in region2
  if (has_region2&& reader->SetRegion(region2)) {
    HashRegionTable* hashes = HashRegionTableAlloc();
    while (reader->GetNextAlignment(bam_alignment)) {
      int index = -1;
      if (bam_alignment.MateRefID == target_ref_id) {
        Scissors::HashesCollection hashes_collection;
        const bool get_hash = 
          LoadHash(bam_alignment.QueryBases, reference, hash_table, hashes, &hashes_collection);
        if (get_hash) {
          const int id = hashes_collection.GetSize() - 1;
          //index = GetHashId(*(hashes_collection.Get(id)), kRequestedBases, reference_header, reference);
          index = GetAlignment(bam_alignment.QueryBases, bam_alignment.QueryBases.size(), *(hashes_collection.Get(id)), reference_header, reference);
        }
        //Align(bam_alignment.QueryBases, aligner, &alignment);
        //index = PickBestAlignment(bam_alignment.Length, alignment, s_ref);
        if (index == -1) { // try the reverse complement sequences
          string reverse;
          GetReverseComplement(bam_alignment.QueryBases, &reverse);
	  //bam_alignment.QueryBases = reverse;
          #ifdef TB_VERBOSE_DEBUG
          fprintf(stderr, "%s\n", reverse.c_str());
          #endif
          Scissors::HashesCollection hashes_collection2;
          const bool get_hash = 
            LoadHash(reverse, reference, hash_table, hashes, &hashes_collection2);
          if (get_hash) {
            const int id = hashes_collection2.GetSize() - 1;
            //index = GetHashId(*(hashes_collection2.Get(id)), kRequestedBases, reference_header, reference);
            index = GetAlignment(reverse, reverse.size(), *(hashes_collection2.Get(id)), reference_header, reference);
          }
          //Align(reverse, aligner, &alignment);
          //index = PickBestAlignment(bam_alignment.Length, alignment, s_ref);
        } // end if (index == -1)
      al.Clear();
      al.bam_alignment = bam_alignment;
      al.hit_insertion = (index == -1) ? false: true;
      al.ins_prefix    = (index == -1) ? "" : s_ref.ref_names[index].substr(8,2);
      StoreInBuffer(&al, al_maps);

      #ifdef TB_VERBOSE_DEBUG
      fprintf(stderr, "SP mapped: %c\tSP:%s\n", (index == -1) ? 'F' : 'T', al.ins_prefix.c_str());
      #endif
      } // end if
    }
    HashRegionTableFree(hashes);
  }
}

void MoveAlInAlmapToAlmaps(
    map<string, Alignment>* al_map1,
    map<string, Alignment>* al_map2,
    vector<map<string, Alignment> >* al_maps,
    BamTools::BamWriter* writer) {
  for (map<string, Alignment>::iterator ite = al_map1->begin();
       ite != al_map1->end(); ++ite) {
    const int32_t ref_id = ite->second.bam_alignment.RefID;
    const int32_t mate_ref_id = ite->second.bam_alignment.MateRefID;
    if (mate_ref_id > ref_id) 
      ((*al_maps)[ref_id])[ite->second.bam_alignment.Name] = ite->second;
    else
      WriteAlignment(&(ite->second), writer);
  }
  al_map1->clear();

  for (map<string, Alignment>::iterator ite = al_map2->begin();
       ite != al_map2->end(); ++ite) {
    const int32_t ref_id = ite->second.bam_alignment.RefID;
    const int32_t mate_ref_id = ite->second.bam_alignment.MateRefID;
    if (mate_ref_id > ref_id) 
      ((*al_maps)[ref_id])[ite->second.bam_alignment.Name] = ite->second;
    else
      WriteAlignment(&(ite->second), writer);
  }
  al_map2->clear();
}

int main(int argc, char** argv) {
  Param param;
  
  if (!ParseArguments(argc, argv, &param)) return 1;

  // Open input bam
  string infilename = param.in_bam;
  string outfilename = param.out_bam;
  BamTools::BamReader reader;
  BamTools::BamWriter writer;
  if (!OpenBams(infilename, outfilename, param.command_line, &reader, &writer)) return 1;

  // Get the ID of target chromosome
  int target_ref_id = -1;
  int previous_ref_id = -1;
  //bool region_set = false;
  vector<map<string, Alignment> > al_maps(reader.GetReferenceCount());
  if (!param.target_ref_name.empty()) {
    target_ref_id = reader.GetReferenceID(param.target_ref_name);
    previous_ref_id = target_ref_id;
    //region_set = true;
  }

  // Special hash
  SpecialHasher sp_hasher;
  sp_hasher.SetFastaName(param.ref_fasta.c_str());
  if (!sp_hasher.Load()) {
    fprintf(stderr,"ERROR: The program cannot load special references.\n");
    return 1;
  }
  const SR_Reference* reference = sp_hasher.GetReference();
  const SR_InHashTable* hash_table = sp_hasher.GetHashTable();
  const SR_RefHeader* reference_header = sp_hasher.GetReferenceHeader();
  HashRegionTable* hashes = HashRegionTableAlloc();

  // Open fasta
  FastaReference fasta;
  LoadReference(param.ref_fasta.c_str(), &fasta);

  // Build SSW aligners for every reference in fasta
  SpecialReference s_ref;
  ConcatenateSpecialReference(&fasta, &s_ref);

  // Build SSW aligner
  //StripedSmithWaterman::Aligner aligner;
  //aligner.SetReferenceSequence(s_ref.concatnated.c_str(), s_ref.concatnated_len);

  // CORE ALGORITHM
  BamTools::BamAlignment bam_alignment;
  map<string, Alignment> al_map1, al_map2;
  map<string, Alignment> *al_map_cur = &al_map1, *al_map_pre = &al_map2;
  StripedSmithWaterman::Alignment alignment;
  Alignment al;

  // Load alignments sitting in other chromosomes and their mates are in the target chr
  if (target_ref_id != -1) {
    string indexfilename = infilename + ".bai";
    if (!reader.OpenIndex(indexfilename)) {
      fprintf(stderr, "Warning: Cannot open the bam index file so creating it......\n");
      reader.CreateIndex();
      fprintf(stderr, "Warning: %s has been created.\n", indexfilename.c_str());
    }
    LoadAlignmentsNotInTargetChr(target_ref_id, reference, hash_table, reference_header, s_ref, &reader, &al_maps);
    const BamTools::RefVector& references = reader.GetReferenceData();
    const BamTools::RefData& ref_data = references.at(target_ref_id);
    reader.SetRegion(target_ref_id, 0, target_ref_id, ref_data.RefLength);
  } else {
    target_ref_id = 0;
    previous_ref_id = 0;
  }

  while (reader.GetNextAlignment(bam_alignment)) {
    if (bam_alignment.RefID != previous_ref_id) { // BAM is in the next chromosome
      #ifdef TB_VERBOSE_DEBUG
      fprintf(stderr, "BAM jumps from chrID: %d to chrID: %d\n", previous_ref_id, bam_alignment.RefID);
      #endif
      MoveAlInAlmapToAlmaps(&al_map1, &al_map2, &al_maps, &writer);
      al_map_cur = &al_map1;
      al_map_pre = &al_map2;
      previous_ref_id = bam_alignment.RefID;
      target_ref_id = bam_alignment.RefID;
    }
    #ifdef TB_VERBOSE_DEBUG
    fprintf(stderr, "%s\n%s\n", bam_alignment.Name.c_str(), bam_alignment.QueryBases.c_str());
    #endif
    int index = -1;
    if (IsProblematicAlignment(bam_alignment)) {
      Scissors::HashesCollection hashes_collection;
      const bool get_hash = 
        LoadHash(bam_alignment.QueryBases, reference, hash_table, hashes, &hashes_collection);
      if (get_hash) {
        const int id = hashes_collection.GetSize() - 1;
        //index = GetHashId(*(hashes_collection.Get(id)), kRequestedBases, reference_header, reference);
        index = GetAlignment(bam_alignment.QueryBases, bam_alignment.QueryBases.size(), *(hashes_collection.Get(id)), reference_header, reference);
      }
      if (index == -1) { // try the reverse complement sequences
        string reverse;
        GetReverseComplement(bam_alignment.QueryBases, &reverse);
	//bam_alignment.QueryBases = reverse;
        #ifdef TB_VERBOSE_DEBUG
        fprintf(stderr, "%s\n", reverse.c_str());
        #endif
        Scissors::HashesCollection hashes_collection2;
        const bool get_hash2 = 
          LoadHash(reverse, reference, hash_table, hashes, &hashes_collection2);
        if (get_hash2) {
          const int id = hashes_collection2.GetSize() - 1;
          //index = GetHashId(*(hashes_collection2.Get(id)), kRequestedBases, reference_header, reference);
          index = GetAlignment(reverse, reverse.size(), *(hashes_collection2.Get(id)), reference_header, reference);
        }
      }
    }
      
    #ifdef TB_VERBOSE_DEBUG
    fprintf(stderr, "SP mapped: %c\n", (index == -1) ? 'F' : 'T');
    #endif

    al.Clear();
    al.bam_alignment = bam_alignment;
    al.hit_insertion = (index == -1) ? false: true;
    al.ins_prefix    = (index == -1) ? "" : s_ref.ref_names[index].substr(8,2);
    if (bam_alignment.RefID == target_ref_id) {
      if (!bam_alignment.IsPaired()) {
        WriteAlignment(&al, &writer);
      } else { //bam_alignment.IsPaired
        if (bam_alignment.RefID == bam_alignment.MateRefID)
	  StoreAlignment(&al, al_map_cur, al_map_pre, &writer);
	else
	  StoreAlignment(&al, &al_maps, &writer);
      }
    } // end if 
  }

  // Close
  WriteAlignment(&al_map1, &writer);
  WriteAlignment(&al_map2, &writer);
  al_map1.clear();
  al_map2.clear();
  reader.Close();
  writer.Close();

  HashRegionTableFree(hashes);
}
