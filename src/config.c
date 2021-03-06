
#include "grid.h"
#include "config.h"

#include <hdf5.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

const int WORK_SPLIT_THRESHOLD = 3;

double get_time_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + (double)ts.tv_nsec / 1000000000;
}

void bl_bounding_box(struct bl_data *bl_data, bool negate,
                     int tstep0, int tstep1,
                     int fstep0, int fstep1,
                     double *uvw_l_min, double *uvw_l_max)
{

    // Check time start and end (TODO - that's simplifying quite a bit)
    double *uvw0 = bl_data->uvw_m + tstep0 * 3;
    double *uvw1 = bl_data->uvw_m + tstep1 * 3;

    // Conversion factor to uvw in lambda
    double scale0 = uvw_m_to_l(1, bl_data->freq[fstep0]),
           scale1 = uvw_m_to_l(1, bl_data->freq[fstep1]);
    if (negate) { scale0 = -scale0; scale1 = -scale1; }

    // Determine bounding box
    int i = 0;
    for (i = 0; i < 3; i++) {
        uvw_l_min[i] = fmin(fmin(uvw0[i]*scale0, uvw0[i]*scale1),
                            fmin(uvw1[i]*scale0, uvw1[i]*scale1));
        uvw_l_max[i] = fmax(fmax(uvw0[i]*scale0, uvw0[i]*scale1),
                            fmax(uvw1[i]*scale0, uvw1[i]*scale1));
    }
}

void bl_bounding_subgrids(struct bl_data *bl_data, bool negate,
                          double lam_sg, double wstep_sg, int a1, int a2,
                          int *sg_min, int *sg_max)
{
    double uvw_l_min[3], uvw_l_max[3];
    bl_bounding_box(bl_data, negate,
                    0, bl_data->time_count-1,
                    0, bl_data->freq_count-1,
                    uvw_l_min, uvw_l_max);

    // Convert into subgrid indices
    sg_min[0] = (int)round(uvw_l_min[0]/lam_sg);
    sg_min[1] = (int)round(uvw_l_min[1]/lam_sg);
    sg_min[2] = (int)round(uvw_l_min[2]/wstep_sg);
    sg_max[0] = (int)round(uvw_l_max[0]/lam_sg);
    sg_max[1] = (int)round(uvw_l_max[1]/lam_sg);
    sg_max[2] = (int)round(uvw_l_max[2]/wstep_sg);

    /* printf("BL %d/%d u %g-%g (%d-%d) v %g-%g (%d-%d) w %g-%g (%d-%d)\n", */
    /*        a1,a2, */
    /*        uvw_l_min[0], uvw_l_max[0], sg_min[0], sg_max[0], */
    /*        uvw_l_min[1], uvw_l_max[1], sg_min[1], sg_max[1], */
    /*        uvw_l_min[2], uvw_l_max[2], sg_min[2], sg_max[2]); */
}

struct worker_prio
{
    int worker;
    int nbl;
};

static int compare_prio_nbl(const void *_w1, const void *_w2)
{
    const struct worker_prio *w1 = (const struct worker_prio *)_w1;
    const struct worker_prio *w2 = (const struct worker_prio *)_w2;
    return w1->nbl > w2->nbl;
}

static int bin_baseline(struct vis_spec *spec, struct bl_data *bl_data,
                        double lam_sg, double wstep_sg,
                        int nsubgrid, int nwlevels,
                        int a1, int a2, int iu, int iv, int iw,
                        double *pmin_w)
{
    assert (iu >= 0 && iu < nsubgrid);
    assert (iv >= 0 && iv < nsubgrid);
    int chunks = 0, tchunk, fchunk;

    double sg_min_u = lam_sg*(iu-nsubgrid/2) - lam_sg/2;
    double sg_min_v = lam_sg*(iv-nsubgrid/2) - lam_sg/2;
    double sg_min_w = wstep_sg*(iw-nwlevels/2) - wstep_sg/2;
    double sg_max_u = lam_sg*(iu-nsubgrid/2) + lam_sg/2;
    double sg_max_v = lam_sg*(iv-nsubgrid/2) + lam_sg/2;
    double sg_max_w = wstep_sg*(iw-nwlevels/2) + wstep_sg/2;
    int ntchunk = spec_time_chunks(spec);
    int nfchunk = spec_freq_chunks(spec);
    double min_w = sg_max_w;

    // Count number of overlapping chunks
    for (tchunk = 0; tchunk < ntchunk; tchunk++) {

        // Check whether time chunk fall into positive u. We use this
        // for deciding whether coordinates are going to get flipped
        // for the entire chunk. This is assuming that a chunk is
        // never big enough that we would overlap an extra subgrid
        // into the negative direction.
        int tstep_mid = tchunk * spec->time_chunk + spec->time_chunk / 2;
        bool positive_u = bl_data->uvw_m[tstep_mid * 3] >= 0;

        // Check frequencies. We adjust step length exponentially so
        // we can jump over non-matching space quicker, see
        // below. This bit of code is likely a bit too smart for its
        // own good!
        int fstep = 1;
        for (fchunk = 0; fchunk < nfchunk; fchunk+=fstep) {

            // Determine chunk bounding box
            double uvw_l_min[3], uvw_l_max[3];
            bl_bounding_box(bl_data, !positive_u,
                            tchunk * spec->time_chunk,
                            fmin(spec->time_count, (tchunk+1) * spec->time_chunk) - 1,
                            fchunk * spec->freq_chunk,
                            fmin(spec->freq_count, (fchunk+fstep) * spec->freq_chunk) - 1,
                            uvw_l_min, uvw_l_max);

            if (uvw_l_min[0] < sg_max_u && uvw_l_max[0] > sg_min_u &&
                uvw_l_min[1] < sg_max_v && uvw_l_max[1] > sg_min_v &&
                uvw_l_min[2] < sg_max_w && uvw_l_max[2] > sg_min_w) {

                if (fstep == 1) {
                    // Found a chunk
                    chunks++;
                    min_w = fmin(min_w, uvw_l_min[2]);
                } else {
                    // Went too fast. Decrease step length, recheck.
                    fstep /= 2;
                    fchunk -= fstep;
                }
            } else {
                // Speed up. Increase step length.
                fchunk -= fstep;
                fstep *= 2;
            }
        }
    }

    if (pmin_w) *pmin_w = min_w;
    return chunks;
}

static int min(int a, int b) { return a > b ? b : a; }
static int max(int a, int b) { return a < b ? b : a; }


// Collect work for a specific subgrid cube
static void collect_work(struct vis_spec *spec, struct bl_data *bl_data,
                         double lam_sg, double wstep_sg,
                         int nsubgrid, int nwlevels,
                         int *sg_mins, int *sg_maxs,
                         int iu, int iv, int iw,
                         int *nchunks, struct subgrid_work_bl **work)
{

    int a1, a2, bl=0;
    for (a1 = 0; a1 < spec->cfg->ant_count; a1++) {
        for (a2 = a1+1; a2 < spec->cfg->ant_count; a2++, bl++) {
            int *sg_min = sg_mins + bl * 3, *sg_max = sg_maxs + bl * 3;

            // Bounds check
            if (!(iv >= nsubgrid/2+sg_min[1] && iv <= nsubgrid/2+sg_max[1] &&
                  iu >= nsubgrid/2+sg_min[0] && iu <= nsubgrid/2+sg_max[0] &&
                  iw >= nwlevels/2+sg_min[2] && iw <= nwlevels/2+sg_max[2]) &&
                !(iv >= nsubgrid/2-sg_max[1] && iv <= nsubgrid/2-sg_min[1] &&
                  iu >= nsubgrid/2-sg_max[0] && iu <= nsubgrid/2-sg_min[0] &&
                  iw >= nwlevels/2-sg_max[2] && iw <= nwlevels/2-sg_min[2]))
                continue;

            // Count actual number of chunks
            double min_w; // lowest w-level touched by a chunk
            struct bl_data *bl_data_bl = bl_data + spec->cfg->ant_count*a2+a1;
            int chunks = bin_baseline(spec, bl_data_bl,
                                      lam_sg, wstep_sg,
                                      nsubgrid, nwlevels,
                                      a1, a2, iu, iv, iw, &min_w);
            if (!chunks)
                continue;

            // Sum up
            *nchunks+=chunks;

            // Make sure we don't add a baseline twice
            assert(!*work || (*work)->a1 != a1 || (*work)->a2 != a2);

            // Find baseline insert position (sort by w so that we
            // maximise locality and minimise redundant w-tower FFTs
            // down the line)
            struct subgrid_work_bl *pos = *work, *prev = NULL;
            while (pos && pos->min_w < min_w) {
                prev = pos; pos = pos->next;
            }

            // Add work structure
            struct subgrid_work_bl *wbl = (struct subgrid_work_bl *)
                malloc(sizeof(struct subgrid_work_bl));
            wbl->a1 = a1; wbl->a2 = a2; wbl->chunks=chunks;
            wbl->next = pos;
            wbl->min_w = min_w;
            /* wbl->sg_min_u = sg_min[0]; */
            /* wbl->sg_min_v = sg_min[1]; */
            /* wbl->sg_min_w = sg_min[2]; */
            /* wbl->sg_max_u = sg_max[0]; */
            /* wbl->sg_max_v = sg_max[1]; */
            /* wbl->sg_max_w = sg_max[2]; */
            wbl->bl_data = bl_data_bl;
            if (prev)
                prev->next = wbl;
            else
                *work = wbl;
            fflush(stdout);


        }
    }

}

// Bin baselines per overlapping subgrid
static void collect_baselines(struct vis_spec *spec, struct bl_data *bl_data,
                              double lam, double lam_sg, double wstep_sg,
                              bool dump_baselines,
                              int **pnchunks, struct subgrid_work_bl ***pbls,
                              int *pnsubgrid, int *pnwlevels)
{

    // Determine baseline bounding boxes
    int nbl_total = spec->cfg->ant_count * (spec->cfg->ant_count - 1) / 2;
    int *sg_mins = (int *)malloc(sizeof(int) * 3 * nbl_total),
        *sg_maxs = (int *)malloc(sizeof(int) * 3 * nbl_total);
    int a1, a2, bl = 0;
    int max_sg_u = 0, max_sg_v = 0, max_sg_w = 0;
    const int nant = spec->cfg->ant_count;
    for (a1 = 0; a1 < nant; a1++) {
        for (a2 = a1+1; a2 < nant; a2++, bl++) {
            int *mins = sg_mins + bl * 3,
                *maxs = sg_maxs + bl * 3;
            bl_bounding_subgrids(bl_data + a1 + nant*a2, false,
                                 lam_sg, wstep_sg, a1, a2,
                                 mins, maxs);
            max_sg_u = max(max_sg_u, max(-mins[0], maxs[0]));
            max_sg_v = max(max_sg_v, max(-mins[1], maxs[1]));
            max_sg_w = max(max_sg_w, max(-mins[2], maxs[2]));
        }
    }

    // Determine number of subgrid bins we need
    int nsubgrid = 2 * (int)ceil(1. / 2 / (lam_sg / lam)) + 3;

    printf("max_sg_u=%d, max_sg_v=%d, max_sg_w=%d nsubgrid=%d\n",
           max_sg_u, max_sg_v, max_sg_w, nsubgrid);
    nsubgrid = min(nsubgrid, max(2*max_sg_u+1, 2*max_sg_v+1));

    // As well as w-levels
    int nwlevels = 2 * max_sg_w + 1;

    // Allocate chunk statistics
    int *nchunks = (int *)calloc(sizeof(int), nsubgrid * nsubgrid * nwlevels);
    struct subgrid_work_bl **bls = (struct subgrid_work_bl **)
        calloc(sizeof(struct subgrid_work_bl *), nsubgrid * nsubgrid * nwlevels);
    int iv, iu, iw;
    #pragma omp parallel for collapse(3) schedule(dynamic,8)
    for (iv = 0; iv < nsubgrid; iv++) {
        for (iu = 0; iu < nsubgrid; iu++) {
            for (iw = 0; iw < nwlevels; iw++) {
                int ix = iw * nsubgrid*nsubgrid + iv * nsubgrid + iu;
                collect_work(spec, bl_data,
                             lam_sg, wstep_sg, nsubgrid, nwlevels,
                             sg_mins, sg_maxs, iu, iv, iw,
                             nchunks + ix, bls + ix);
            }
        }
    }

    free(sg_mins); free(sg_maxs);

    // Produce dump if requested
    if (dump_baselines) {
        printf("Baseline bins:\n---\niu,iv,iw,chunks\n");
        for (iv = 0; iv < nsubgrid; iv++) {
            for (iu = nsubgrid/2; iu < nsubgrid; iu++) {
                for (iw = 0; iw < nwlevels; iw++) {
                    int chunks = 0; struct subgrid_work_bl *bl;
                    for (bl = bls[nsubgrid*nsubgrid*iw+nsubgrid*iv+iu];
                         bl; bl=bl->next) {
                        chunks += bl->chunks;
                        printf("%g ", bl->min_w);
                    }
                    if (chunks) {
                        printf("%d,%d,%d,%d\n", iu, iv, iw, chunks);
                    }
                }
            }
        }
        printf("---\n");
    }

    *pnchunks = nchunks;
    *pbls = bls;
    *pnsubgrid = nsubgrid;
    *pnwlevels = nwlevels;
}

// Pop given number of baselines from the start of the linked list
static struct subgrid_work_bl *pop_chunks(struct subgrid_work_bl **bls, int n, int *nchunks)
{
    struct subgrid_work_bl *first = *bls;
    struct subgrid_work_bl *bl = *bls;
    *nchunks = 0;
    assert(n >= 1);
    if (!bl) return bl;
    while (n > bl->chunks && bl->next) {
      *nchunks += bl->chunks;
      n-=bl->chunks;
      bl = bl->next;
    }
    *nchunks += bl->chunks;
    *bls = bl->next;
    bl->next = NULL;
    return first;
}

static bool generate_subgrid_work_assignment(struct work_config *cfg)
{
    struct vis_spec *spec = &cfg->spec;


    printf("Covering %d time steps, %d channels, %d baselines\n",
           cfg->spec.time_count, cfg->spec.freq_count,
           cfg->spec.cfg->ant_count * (cfg->spec.cfg->ant_count - 1) / 2);

    // Count visibilities per sub-grid
    printf("Binning chunks...\n");
    int *nbl; struct subgrid_work_bl **bls;
    double start = get_time_ns();
    const double lam = config_lambda(cfg);
    int nsubgrid, nwlevels;
    collect_baselines(spec, cfg->bl_data, lam,
                      cfg->sg_step / cfg->theta,
                      cfg->sg_step_w * cfg->wstep,
                      cfg->config_dump_baseline_bins,
                      &nbl, &bls, &nsubgrid, &nwlevels);
    printf(" %g s\n", get_time_ns() - start);

    // Count how many sub-grids actually have visibilities
    int npop = 0, nbl_total = 0, nbl_max = 0;
    int iu, iv, iw;
    for (iw = 0; iw < nwlevels; iw++) {
        for (iv = 0; iv < nsubgrid; iv++) {
            for (iu = 0; iu < nsubgrid; iu++) {
                int n = nbl[iw * nsubgrid*nsubgrid + iv * nsubgrid + iu];
                if (n) npop++;
                nbl_total += n;
                nbl_max = max(n, nbl_max);
            }
        }
    }

    double coverage = (double)npop * cfg->sg_step * cfg->sg_step / lam / lam;

    // We don't want bins that are too full compared to the average -
    // determine at what point we're going to split them.
    int work_max_nbl = (int)fmax(WORK_SPLIT_THRESHOLD * nbl_total / npop,
                                 (nbl_max + cfg->subgrid_workers - 1) / cfg->subgrid_workers);
    printf("%d subgrid baseline bins (%.3g%% coverage), %.5g average chunks per subgrid, "
           "splitting above %d\n",
           npop, coverage*100, (double)nbl_total / npop, work_max_nbl);

    // Now count again how much work we have total, and per
    // column. Note that we ignore grid data at u < 0, as transferring
    // half the grid is enough to reconstruct a real-valued image.
    int nwork = 0, max_work_column = 0;
    for (iw = 0; iw < nwlevels; iw++) {
        for (iu = 0; iu < nsubgrid; iu++) {
            int nwork_start = nwork;
            for (iv = 0; iv < nsubgrid; iv++) {
                int nv = nbl[iw * nsubgrid*nsubgrid + iv * nsubgrid + iu];
                nwork += (nv + work_max_nbl - 1) / work_max_nbl;
            }
            // How much work in this column?
            if (nwork - nwork_start > max_work_column)
                max_work_column = nwork-nwork_start;
        }
    }

    // Allocate work description
    cfg->subgrid_max_work = (nwork + cfg->subgrid_workers - 1) / cfg->subgrid_workers;
    cfg->subgrid_work = (struct subgrid_work *)
        calloc(sizeof(struct subgrid_work), cfg->subgrid_workers * cfg->subgrid_max_work);
    printf("%d split subgrid baseline bins, %d per worker\n", nwork, cfg->subgrid_max_work);

    // Worker priority order for acquiring new work
    struct worker_prio *worker_prio = malloc(sizeof(worker_prio) * cfg->subgrid_workers);
    int i;
    for (i = 0; i < cfg->subgrid_workers; i++) {
        worker_prio[i].worker = i;
        worker_prio[i].nbl = 0;
    }

    // Go through columns and assign work
    int iworker = 0, iwork = 0;
    for (iw = 0; iw < nwlevels; iw++) {
      for (iu = 0; iu < nsubgrid; iu++) {

        // Generate column of work
        int start_bl;
        for (iv = 0; iv < nsubgrid; iv++) {
            int ix = iw * nsubgrid*nsubgrid + iv * nsubgrid + iu;
            for (start_bl = 0; start_bl < nbl[ix]; start_bl += work_max_nbl) {

                // Assign work to next worker
                struct subgrid_work *work =
                    cfg->subgrid_work + iworker * cfg->subgrid_max_work + iwork;

                work->iu = iu - nsubgrid/2;
                work->iv = iv - nsubgrid/2;
                work->iw = iw - nwlevels/2;
                work->subgrid_off_u = cfg->sg_step * work->iu;
                work->subgrid_off_v = cfg->sg_step * work->iv;
                work->subgrid_off_w = cfg->sg_step_w * work->iw;
                work->bls = pop_chunks(&bls[ix], work_max_nbl, &work->nbl);

                // Save back how many chunks were assigned
                worker_prio[iworker].nbl += work->nbl;
                iworker++;
                if (iworker >= cfg->subgrid_workers) {
                    iworker = 0;
                    iwork++;
                }

            }
        }
      }
    }

    // Determine average
    int64_t sum = 0;
    for (i = 0; i < cfg->subgrid_workers; i++) {
        sum += worker_prio[i].nbl;
    }
    int average = sum / cfg->subgrid_workers;

    // Swap work to even out profile
    bool improvement; int nswaps = 0;
    do {
        improvement = false;

        // Sort worker priority
        qsort(worker_prio, cfg->subgrid_workers, sizeof(void *), compare_prio_nbl);

        // Walk through worker pairs
        int prio1 = 0, prio2 = cfg->subgrid_workers - 1;
        while(prio1 < prio2) {
            int diff = worker_prio[prio2].nbl - worker_prio[prio1].nbl;
            int worker1 = worker_prio[prio1].worker;
            int worker2 = worker_prio[prio2].worker;

            // Find a work item to switch
            int iwork;
            struct subgrid_work *work1 = cfg->subgrid_work + worker1 * cfg->subgrid_max_work;
            struct subgrid_work *work2 = cfg->subgrid_work + worker2 * cfg->subgrid_max_work;
            int best = -1, best_diff = diff;
            for (iwork = 0; iwork < cfg->subgrid_max_work; iwork++) {
                int wdiff = work2[iwork].nbl - work1[iwork].nbl;
                if (abs(diff - 2*wdiff) < best_diff) {
                    best = iwork; best_diff = abs(diff - 2*wdiff);
                }
            }

            // Found a swap?
            if (best != -1) {

                struct subgrid_work w = work1[best];
                work1[best] = work2[best];
                work2[best] = w;

                worker_prio[prio1].nbl += work1[best].nbl - work2[best].nbl;
                worker_prio[prio2].nbl += work2[best].nbl - work1[best].nbl;

                improvement = true;
                nswaps++;
                break;
            }

            // Step workers. Keep the one that is further away from the
            // average.
            if (abs(worker_prio[prio2].nbl - average) >
                abs(worker_prio[prio1].nbl - average)) {
                prio1++;
            } else {
                prio2--;
            }
        }

    } while(improvement);

    // Statistics
    int min_vis = INT_MAX, max_vis = 0;
    cfg->iu_min = INT_MAX; cfg->iu_max = INT_MIN;
    cfg->iv_min = INT_MAX; cfg->iv_max = INT_MIN;
    for (i = 0; i < cfg->subgrid_workers; i++) {
        int j; int vis = 0;
        for (j = 0; j < cfg->subgrid_max_work; j++) {
            struct subgrid_work *work = cfg->subgrid_work + i* cfg->subgrid_max_work+j;
            if (work->iu < cfg->iu_min) cfg->iu_min = work->iu;
            if (work->iu > cfg->iu_max) cfg->iu_max = work->iu;
            if (work->iv < cfg->iv_min) cfg->iv_min = work->iv;
            if (work->iv > cfg->iv_max) cfg->iv_max = work->iv;
            vis += work->nbl;
            //printf("%d ", work->nbl);
        }
        //printf(" -> %d %d\n", vis, worker_prio[i].nbl);
        min_vis = fmin(vis, min_vis);
        max_vis = fmax(vis, max_vis);
    }
    printf("Assigned workers %d chunks min, %d chunks max (after %d swaps)\n", min_vis, max_vis, nswaps);

    if (cfg->config_dump_subgrid_work) {
        printf("Subgrid work (after swaps):\n---\nworker,work,chunks,iu,iv,iw\n");
        for (i = 0; i < cfg->subgrid_workers; i++) {
            int j;
            for (j = 0; j < cfg->subgrid_max_work; j++) {
                struct subgrid_work *work = cfg->subgrid_work + i* cfg->subgrid_max_work+j;
                if (work->nbl > 0) {
                    printf("%d,%d,%d,%d,%d,%d\n", i,j, work->nbl,
                           work->iu, work->iv, work->iw);
                }
            }
        }
        puts("---");
    }

    return true;
}

static bool generate_facet_work_assignment(struct work_config *cfg)
{
    if (cfg->facet_workers == 0) return true;

    // This is straightforward: We just assume that all facets within
    // the field of view are set. Note that theta is generally larger
    // than the FoV, so this won't cover the entire image.
    double yB = (double)cfg->recombine.yB_size / cfg->recombine.image_size;
    int nfacet = 2 * ceil(cfg->spec.fov / cfg->theta / yB / 2 - 0.5) + 1;
    printf("%dx%d facets covering %g FoV (facet %g, grid theta %g)\n",
           nfacet, nfacet, cfg->spec.fov, cfg->theta * yB, cfg->theta);

    // Allocate work array
    cfg->facet_max_work = (nfacet * nfacet + cfg->facet_workers - 1) / cfg->facet_workers;
    cfg->facet_count = nfacet * nfacet;
    cfg->facet_work = (struct facet_work *)
        calloc(sizeof(struct facet_work), cfg->facet_workers * cfg->facet_max_work);

    int i;
    for (i = 0; i < nfacet * nfacet; i++) {
        int iworker = i % cfg->facet_workers, iwork = i / cfg->facet_workers;
        struct facet_work *work = cfg->facet_work + cfg->facet_max_work * iworker + iwork;
        work->il = (i / nfacet) - nfacet/2;
        work->im = (i % nfacet) - nfacet/2;
        work->facet_off_l = work->il * cfg->recombine.yB_size;
        work->facet_off_m = work->im * cfg->recombine.yB_size;
        work->set = true;
    }

    return true;
}

static bool generate_full_redistribute_assignment(struct work_config *cfg)
{

    // No visibilities involved, so generate work assignment where we
    // simply redistribute all data from a number of facets matching
    // the number of facet workers.
    assert(!cfg->spec.time_count);

    if (cfg->subgrid_workers > 0) {
        int nsubgrid = cfg->recombine.image_size / cfg->sg_step;
        int subgrid_work = nsubgrid * nsubgrid;
        cfg->subgrid_max_work = (subgrid_work + cfg->subgrid_workers - 1) / cfg->subgrid_workers;
        cfg->subgrid_work = (struct subgrid_work *)
            calloc(sizeof(struct subgrid_work), cfg->subgrid_max_work * cfg->subgrid_workers);
        int i;
        for (i = 0; i < subgrid_work; i++) {
            struct subgrid_work *work = cfg->subgrid_work + i;
            work->iu = i / nsubgrid;
            work->iv = i % nsubgrid;
            work->iw = 0;
            work->subgrid_off_u = work->iu * cfg->sg_step;
            work->subgrid_off_v = work->iv * cfg->sg_step;
            work->subgrid_off_w = work->iw * cfg->sg_step_w;
            work->nbl = 1;
            // Dummy 0-0 baseline
            work->bls = (struct subgrid_work_bl *)calloc(sizeof(struct  subgrid_work_bl), 1);
        }
        cfg->iu_min = cfg->iv_min = 0;
        cfg->iu_max = cfg->iv_max = nsubgrid-1;
    }

    if (cfg->facet_workers > 0) {
        int nfacet = cfg->recombine.image_size / cfg->recombine.yB_size;
        cfg->facet_max_work = (nfacet * nfacet + cfg->facet_workers - 1) / cfg->facet_workers;
        cfg->facet_count = nfacet * nfacet;
        cfg->facet_work = (struct facet_work *)
            calloc(sizeof(struct facet_work), cfg->facet_max_work * cfg->facet_workers);
        int i;
        for (i = 0; i < nfacet * nfacet; i++) {
            int iworker = i % cfg->facet_workers, iwork = i / cfg->facet_workers;
            struct facet_work *work = cfg->facet_work + cfg->facet_max_work * iworker + iwork;
            work->il = i / nfacet;
            work->im = i % nfacet;
            work->facet_off_l = work->il * cfg->recombine.yB_size;
            work->facet_off_m = work->im * cfg->recombine.yB_size;
            work->set = true;
        }
    }

    return true;
}

void config_init(struct work_config *cfg)
{

    // Initialise structure
    memset(cfg, 0, sizeof(*cfg));
    cfg->gridder.x0 = 0.5;
    cfg->w_gridder.x0 = 0.5;
    cfg->config_dump_baseline_bins = false;
    cfg->config_dump_subgrid_work = false;
    cfg->produce_parallel_cols = false;
    cfg->produce_retain_bf = true;
    cfg->produce_batch_rows = 16;
    cfg->produce_queue_length = 4;
    cfg->vis_skip_metadata = true;
    cfg->vis_bls_per_task = 256;
    cfg->vis_subgrid_queue_length = 256;
    cfg->vis_task_queue_length = 96;
    cfg->vis_chunk_queue_length = 4096;
    cfg->vis_writer_count = 2;
    cfg->vis_fork_writer = false;
    cfg->vis_check_existing = false;
    cfg->vis_checks = 16384;
    cfg->grid_checks = 4096;
    cfg->vis_max_error = 1;
    cfg->vis_round_to_wplane = false;

    cfg->statsd_socket = -1;
    cfg->statsd_rate = 1;
}

static void print_power2(int x)
{
    if (x >= 1024 * 1024 && x % (1024 * 1024) == 0) {
        printf("%dM", x / 1024 / 1024);
    } else if (x >= 1024 && x % 1024 == 0) {
        printf("%dk", x / 1024);
    } else {
        printf("%d", x);
    }
}

bool config_set(struct work_config *cfg,
                int image_size, int subgrid_spacing,
                char *pswf_file,
                int yB_size, int yN_size, int yP_size,
                int xA_size, int xM_size, int xMxN_yP_size)
{

    // Set recombination configuration
    printf("\nInitialising recombination (image size "); print_power2(image_size);
    printf(", facet FFT "); print_power2(yP_size);
    printf(", subgrid FFT "); print_power2(xM_size);
    printf(")...\n");
    if (!recombine2d_set_config(&cfg->recombine, image_size, subgrid_spacing, pswf_file,
                                yB_size, yN_size, yP_size,
                                xA_size, xM_size, xMxN_yP_size))
        return false;
    cfg->sg_step = cfg->recombine.xA_size;

    return true;
}

void config_free(struct work_config *cfg)
{
    free(cfg->vis_path);
    free(cfg->facet_work);
    free(cfg->gridder.data); cfg->gridder.data = NULL;
    free(cfg->gridder.corr); cfg->gridder.corr = NULL;
    free(cfg->w_gridder.data); cfg->w_gridder.data = NULL;
    free(cfg->w_gridder.corr); cfg->w_gridder.corr = NULL;

    int i;
    for (i = 0; i < cfg->subgrid_workers * cfg->subgrid_max_work; i++) {
        while (cfg->subgrid_work[i].bls) {
            struct subgrid_work_bl *bl = cfg->subgrid_work[i].bls;
            cfg->subgrid_work[i].bls = cfg->subgrid_work[i].bls->next;
            free(bl);
        }
    }
    free(cfg->subgrid_work);
    free(cfg->spec.ha_sin);
    free(cfg->spec.ha_cos);

    if (cfg->statsd_socket != -1) close(cfg->statsd_socket);
    cfg->statsd_socket = -1;
}

// Make baseline specification. Right now this is the same for every
// baseline, but this will change for baseline dependent averaging.
void vis_spec_to_bl_data(struct bl_data *bl, struct vis_spec *spec,
                         int a1, int a2)
{
    int i;

    // Create baseline structure
    bl->time_count = spec->time_count;
    bl->time = (double *)malloc(sizeof(double) * spec->time_count);
    for (i = 0; i < spec->time_count; i++) {
        bl->time[i] = spec->time_start + spec->time_step * i;
    }
    bl->uvw_m = (double *)malloc(sizeof(double) * spec->time_count * 3);
    for (i = 0; i < spec->time_count; i++) {
        ha_to_uvw_sc(spec->cfg, a1, a2,
                     spec->ha_sin[i], spec->ha_cos[i],
                     spec->dec_sin, spec->dec_cos,
                     bl->uvw_m + i*3);
    }
    bl->freq_count = spec->freq_count;
    bl->freq = (double *)malloc(sizeof(double) * spec->freq_count);
    for (i = 0; i < spec->freq_count; i++) {
        bl->freq[i] = spec->freq_start + spec->freq_step * i;
    }
    bl->antenna1 = a1;
    bl->antenna2 = a2;

}

void config_set_visibilities(struct work_config *cfg,
                             struct vis_spec *spec,
                             const char *vis_path)
{
    // Copy
    cfg->spec = *spec;
    if (vis_path)
        cfg->vis_path = strdup(vis_path);

    // Calculate grid step lengths
    double max_lm = spec->fov / 2;
    cfg->theta = max_lm / cfg->gridder.x0;
    assert(max_lm < 0.5);
    double max_n = 1 - sqrt(1 - 2*max_lm*max_lm);
    cfg->wstep = cfg->w_gridder.x0 / max_n;
    printf("Grid resolution u/v: %g lambda, w: %g lambda\n", 1/cfg->theta, cfg->wstep);

    // Calculate uvw cube split
    assert(cfg->recombine.image_size != 0); // Must be set previously
    cfg->sg_step = cfg->recombine.xA_size;
    cfg->sg_step_w = 1;

    // Cache cosinus + sinus values
    cfg->spec.ha_sin = (double *)malloc(sizeof(double) * cfg->spec.time_count);
    cfg->spec.ha_cos = (double *)malloc(sizeof(double) * cfg->spec.time_count);
    int it;
    for (it = 0; it < cfg->spec.time_count; it++) {
        double t = spec->time_start + spec->time_step * it;
        cfg->spec.ha_sin[it] = sin(t * M_PI / 12);
        cfg->spec.ha_cos[it] = cos(t * M_PI / 12);
    }
    cfg->spec.dec_sin = sin(cfg->spec.dec);
    cfg->spec.dec_cos = cos(cfg->spec.dec);

    // Calculate baseline data (UVWs)
    printf("Calculating UVW...\n");
    const int nant = spec->cfg->ant_count;
    cfg->bl_data = (struct bl_data *)calloc(sizeof(struct bl_data), nant * nant);
    int a1, a2;
    for (a1 = 0; a1 < nant; a1++) {
        for (a2 = a1+1; a2 < nant; a2++) {
            vis_spec_to_bl_data(cfg->bl_data + a1 + nant*a2, &cfg->spec, a1, a2);
        }
    }

}

bool config_set_degrid(struct work_config *cfg, const char *gridder_path,
                       double gridder_x0, int downsample)
{
    if (gridder_path) {

        // Clear existing data, if any
        free(cfg->gridder.data); free(cfg->gridder.corr);
        memset(&cfg->gridder, 0, sizeof(struct sep_kernel_data));

        // Load gridder
        if (load_sep_kern(gridder_path, &cfg->gridder, true)) {
            return false;
        }
        // Override x0
        if (gridder_x0 != 0) {
            cfg->gridder.x0 = gridder_x0;
        }

        // Reduce oversampling if requested
        if (downsample) {
            cfg->gridder.oversampling /= downsample;
            printf("Downsampling gridder to %d", cfg->gridder.oversampling);
            int i;
            for (i = 1; i < cfg->gridder.oversampling; i++) {
                memcpy(cfg->gridder.data + i * cfg->gridder.stride,
                       cfg->gridder.data + i * downsample * cfg->gridder.stride,
                       sizeof(double) * cfg->gridder.size);
            }
        }

        // Need to rescale? This is linear, therefore worth a
        // warning. Might want to do a "sinc" interpolation instead at
        // some point? Could be more appropriate.
        const int ncorr = cfg->gridder.corr_size, image_size = cfg->recombine.image_size;
        if (ncorr != image_size) {
            if (ncorr % image_size != 0) {
                fprintf(stderr, "WARNING: Rescaling grid correction from %d to %d points!\n",
                        ncorr, image_size);
            }
            int i;
            double *grid_correction = (double *)malloc(sizeof(double) * image_size);
            for (i = 0; i < image_size; i++) {
                double j = (double)i * ncorr / image_size;
                int j0 = (int)floor(j),  j1 = (j0 + 1) % ncorr;
                double w = j - j0;
                grid_correction[i] = (1 - w) * cfg->gridder.corr[j0] + w * cfg->gridder.corr[j1];
            }
            free(cfg->gridder.corr);
            cfg->gridder.corr = grid_correction;
            cfg->gridder.corr_size = image_size;
        }

    }

    return true;
}

bool config_set_statsd(struct work_config *cfg,
                       const char *node, const char *service)
{
    if (cfg->statsd_socket != -1) close(cfg->statsd_socket);
    cfg->statsd_socket = -1;

    // Resolve statsd address
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    int ret = getaddrinfo(node, service, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "ERROR: Could not resolve statsd address (%s)", gai_strerror(ret));
        return false;
    }

    // Create socket
    struct addrinfo *addr = NULL;
    for (addr = result; addr; addr = addr->ai_next) {
        // Attempt to create socket
        cfg->statsd_socket = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (cfg->statsd_socket == -1)
            continue;

        // And connect
        if (connect(cfg->statsd_socket, addr->ai_addr, addr->ai_addrlen) != -1)
            break;

        close(cfg->statsd_socket); cfg->statsd_socket = -1;
    }
    if (cfg->statsd_socket == -1) {
        fprintf(stderr, "ERROR: Could not create statsd socket (%s)", strerror(errno));
        freeaddrinfo(result);
        return false;
    }

    freeaddrinfo(result);

    // Initialise stats
    printf("Opened statsd connection to %s:%s\n", node, service);
    return true;
}

void config_send_statsd(struct work_config *cfg, const char *stat)
{
    if (cfg->statsd_socket == -1)
        return;

    //printf("stats: %s\n", stat);

    if (write(cfg->statsd_socket, stat, strlen(stat)) != strlen(stat)) {
        fprintf(stderr, "ERROR: Failed to send to statsd (%s)\n", strerror(errno));
        close(cfg->statsd_socket);
        cfg->statsd_socket = -1;
    }
}

void config_load_facets(struct work_config *cfg,
                        const char *path_fmt,
                        const char *hdf5)
{

    int i;
    for (i = 0; i < cfg->facet_workers * cfg->facet_max_work; i++) {
        struct facet_work *work = cfg->facet_work + i;
        if (!work->set) continue;
        char path[256];
        snprintf(path, 256, path_fmt, work->im, work->il);
        work->path = strdup(path);
        work->hdf5 = hdf5 ? strdup(hdf5) : NULL;
    }

}

void config_check_subgrids(struct work_config *cfg,
                           double threshold, double fct_threshold,
                           double degrid_threshold,
                           const char *check_fmt,
                           const char *check_fct_fmt,
                           const char *check_degrid_fmt,
                           const char *hdf5)
{

    int i;
    for (i = 0; i < cfg->subgrid_workers * cfg->subgrid_max_work; i++) {
        struct subgrid_work *work = cfg->subgrid_work + i;
        if (!work->nbl) continue;
        char path[256];
        if (check_fmt) {
            snprintf(path, 256, check_fmt, work->iv, work->iu);
            work->check_path = strdup(path);
        }
        if (check_fct_fmt) {
            snprintf(path, 256, check_fct_fmt, work->iv, work->iu);
            work->check_fct_path = strdup(path);
        }
        if (check_degrid_fmt) {
            snprintf(path, 256, check_degrid_fmt, work->iv, work->iu);
            work->check_degrid_path = strdup(path);
        }
        work->check_hdf5 = hdf5 ? strdup(hdf5) : NULL;
        work->check_threshold = threshold;
        work->check_fct_threshold = fct_threshold;
        work->check_degrid_threshold = degrid_threshold;
    }

}

bool config_assign_work(struct work_config *cfg,
                        int facet_workers, int subgrid_workers)
{
    cfg->facet_workers = facet_workers;
    cfg->subgrid_workers = subgrid_workers;

    // Generate work assignments
    if (cfg->spec.time_count) {
        printf("\nGenerating work assignments...\n");
        if (!generate_facet_work_assignment(cfg))
            return false;
        if (!generate_subgrid_work_assignment(cfg))
            return false;
    } else {
        if (!generate_full_redistribute_assignment(cfg))
            return false;
    }

    // Warn if we have multiple facets per worker
    if (cfg->facet_max_work > 1) {
        printf("WARNING: %d facets, but only %d workers. This means that facets\n"
               "         compete for send slots, which can cause deadlock unless\n"
               "         the subgrid queue is over-dimensioned. Consider more ranks!\n",
               cfg->facet_count, cfg->facet_workers);
    }

    return true;
}

void config_set_sources(struct work_config *cfg, int count, unsigned int seed)
{

    // Determine portion of the image that can be assumed to be
    // precise with respect to our gridding function
    const int image_size = cfg->recombine.image_size;
    const double image_x0 = cfg->spec.fov / cfg->theta / 2;
    const int image_x0_size = (int)floor(2 * image_x0 * image_size);

    // Allocate
    cfg->source_count = count;
    free(cfg->source_xy); free(cfg->source_lmn); free(cfg->source_corr);
    cfg->source_xy = (double *)malloc(sizeof(double) * count * 2);
    cfg->source_lmn = (double *)malloc(sizeof(double) * count * 3);
    cfg->source_corr = (double *)malloc(sizeof(double) * count);
    cfg->source_energy = (double)count / image_size / image_size;

    // Start making sources using fixed random seed (so all ranks
    // create the same ones)
    int i;
    for (i = 0; i < count; i++) {
        // Choose on-grid positions
        int il = (int)(rand_r(&seed) % image_x0_size) - image_x0_size / 2;
        int im = (int)(rand_r(&seed) % image_x0_size) - image_x0_size / 2;
        cfg->source_xy[2*i+0] = il; cfg->source_xy[2*i+1] = im;
        // Determine coordinates in l/m/n system
        double l = il * cfg->theta / image_size;
        double m = im * cfg->theta / image_size;
        double n = sqrt(1 - l*l - m*m) - 1;
        cfg->source_lmn[3*i+0] = l;
        cfg->source_lmn[3*i+1] = m;
        cfg->source_lmn[3*i+2] = n;
        // Get grid correction to apply
        if (cfg->gridder.corr) {
            cfg->source_corr[i] =
                cfg->gridder.corr[(il + image_size) % image_size] *
                cfg->gridder.corr[(im + image_size) % image_size];
            // Could cause divisions by zero down the line
            assert(cfg->source_corr[i] != 0);
        } else {
            cfg->source_corr[i] = 1;
        }
    }

}

bool create_bl_groups(hid_t vis_group, struct work_config *work_cfg, int worker)
{
    struct vis_spec *spec = &work_cfg->spec;
    struct ant_config *cfg = spec->cfg;

    // Map baselines to work
    struct subgrid_work **bl_work = NULL;
    if (worker >= 0) {
        bl_work = (struct subgrid_work **)
            calloc(sizeof(struct subgrid_work *), cfg->ant_count * cfg->ant_count);
        struct subgrid_work *work = work_cfg->subgrid_work + worker * work_cfg->subgrid_max_work;
        int iwork;
        for (iwork = 0; iwork < work_cfg->subgrid_max_work; iwork++) {
            if (work[iwork].nbl == 0) continue;
            struct subgrid_work_bl *bl;
            for (bl = work[iwork].bls; bl; bl = bl->next) {
                // Note this might overlap (here: overwrite). We are just
                // interested in an example below.
                bl_work[bl->a1 * cfg->ant_count + bl->a2] = &work[iwork];
            }
        }
    }

    int a1, a2;
    int ncreated = 0;
    uint64_t nvis = 0;
    double create_start = get_time_ns();
    for (a1 = 0; a1 < cfg->ant_count; a1++) {
        // Progress message
        if (a1 % 32 == 0) { printf("%d ", a1); fflush(stdout); }

        hid_t a1_g = 0;
        for (a2 = a1+1; a2 < cfg->ant_count; a2++) {
            if (bl_work) {
                struct subgrid_work *bw = bl_work[a1 * cfg->ant_count + a2];
                if (!bw) continue;
            }

            // Create outer antenna group, if not already done so
            if (!a1_g) {
                char a1name[12];
                sprintf(a1name, "%d", a1);
                a1_g = H5Gcreate(vis_group, a1name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                if (a1_g < 0) {
                    fprintf(stderr, "Could not open '%s' antenna group!\n", a1name);
                    return false;
                }
            }

            // Create inner antenna group
            char a2name[12];
            sprintf(a2name, "%d", a2);
            hid_t a2_g = H5Gcreate(a1_g, a2name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            if (a2_g < 0) {
                fprintf(stderr, "Could not open '%s' antenna group!\n", a2name);
                H5Gclose(a1_g);
                return false;
            }

            // Create baseline structure (TODO: baseline-dependent averaging...)
            struct bl_data bl;
            vis_spec_to_bl_data(&bl, spec, a1, a2);

            // Write to visibility group
            if (!create_vis_group(a2_g, spec->freq_chunk, spec->time_chunk,
                                  work_cfg->vis_skip_metadata, &bl)) {
                H5Gclose(a2_g); H5Gclose(a1_g);
                return 1;
            }

            // Statistics & cleanups
            ncreated++;
            nvis += bl.time_count * bl.freq_count;
            free(bl.time); free(bl.uvw_m); free(bl.freq);
            H5Gclose(a2_g);
        }
        if (a1_g) H5Gclose(a1_g);
    }

    printf("\ndone in %.2fs, %d groups for up to %"PRIu64" visibilities (~%.3f GB) created\n",
           get_time_ns() -create_start, ncreated, nvis, 16. * nvis / 1000000000);

    return true;
}
