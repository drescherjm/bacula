/*
   Bacula® - The Network Backup Solution

   Copyright (C) 2001-2007 Free Software Foundation Europe e.V.

   The main author of Bacula is Kern Sibbald, with contributions from
   many others, a complete list can be found in the file AUTHORS.
   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version two of the GNU General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.

   Bacula® is a registered trademark of John Walker.
   The licensor of Bacula is the Free Software Foundation Europe
   (FSFE), Fiduciary Program, Sumatrastrasse 25, 8006 Zürich,
   Switzerland, email:ftf@fsfeurope.org.
*/
/*
 *
 *   Bacula Director -- next_vol -- handles finding the next
 *    volume for append.  Split out of catreq.c August MMIII
 *    catalog request from the Storage daemon.

 *     Kern Sibbald, March MMI
 *
 *   Version $Id$
 */

#include "bacula.h"
#include "dird.h"

static bool get_scratch_volume(JCR *jcr, MEDIA_DBR *mr, bool InChanger);


/*
 *  Items needed:
 *   mr.PoolId must be set
 *   jcr->wstore
 *   jcr->db
 *   jcr->pool
 *   MEDIA_DBR mr with PoolId set
 *   create -- whether or not to create a new volume
 */
int find_next_volume_for_append(JCR *jcr, MEDIA_DBR *mr, int index,             
                                bool create, bool prune)
{
   int retry = 0;
   bool ok;
   bool InChanger;
   STORE *store = jcr->wstore;

   bstrncpy(mr->MediaType, store->media_type, sizeof(mr->MediaType));
   Dmsg2(150, "find_next_vol_for_append: PoolId=%d, MediaType=%s\n", (int)mr->PoolId, mr->MediaType);
   /*
    * If we are using an Autochanger, restrict Volume
    *   search to the Autochanger on the first pass
    */
   InChanger = store->autochanger;
   /*
    * Find the Next Volume for Append
    */
   db_lock(jcr->db);
   for ( ;; ) {
      bstrncpy(mr->VolStatus, "Append", sizeof(mr->VolStatus));  /* want only appendable volumes */
      /*
       *  1. Look for volume with "Append" status.
       */
      ok = db_find_next_volume(jcr, jcr->db, index, InChanger, mr);

      if (!ok) {
         Dmsg4(050, "after find_next_vol ok=%d index=%d InChanger=%d Vstat=%s\n",
               ok, index, InChanger, mr->VolStatus);
         /*
          * 2. Try finding a recycled volume
          */
         ok = find_recycled_volume(jcr, InChanger, mr);
         Dmsg2(150, "find_recycled_volume ok=%d FW=%d\n", ok, mr->FirstWritten);
         if (!ok) {
            /*
             * 3. Try recycling any purged volume
             */
            ok = recycle_oldest_purged_volume(jcr, InChanger, mr);
            if (!ok) {
               /*
                * 4. Try pruning Volumes
                */
               Dmsg0(150, "Call prune_volumes\n");
               if (prune) {
                  prune_volumes(jcr, mr);
               }
               ok = recycle_oldest_purged_volume(jcr, InChanger, mr);
               if (!ok) {
                  Dmsg4(050, "after prune volumes_vol ok=%d index=%d InChanger=%d Vstat=%s\n",
                        ok, index, InChanger, mr->VolStatus);
                  /*
                   * 5. Try pulling a volume from the Scratch pool
                   */ 
                  ok = get_scratch_volume(jcr, mr, InChanger);
               }
               /*
                * If we are using an Autochanger and have not found
                * a volume, retry looking for any volume. 
                */
               if (InChanger) {
                  InChanger = false;
                  if (!ok) {
                     continue;           /* retry again accepting any volume */
                  }
               }
            }
         }


         if (!ok && create) {
            /*
             * 6. Try "creating" a new Volume
             */
            ok = newVolume(jcr, mr);
         }
         /*
          *  Look at more drastic ways to find an Appendable Volume
          */
         if (!ok && (jcr->pool->purge_oldest_volume ||
                     jcr->pool->recycle_oldest_volume)) {
            Dmsg2(200, "No next volume found. PurgeOldest=%d\n RecyleOldest=%d",
                jcr->pool->purge_oldest_volume, jcr->pool->recycle_oldest_volume);
            /* Find oldest volume to recycle */
            ok = db_find_next_volume(jcr, jcr->db, -1, InChanger, mr);
            Dmsg1(400, "Find oldest=%d\n", ok);
            if (ok && prune) {
               UAContext *ua;
               Dmsg0(400, "Try purge.\n");
               /*
                * 7.  Try to purging oldest volume only if not UA calling us.
                */
               ua = new_ua_context(jcr);
               if (jcr->pool->purge_oldest_volume && create) {
                  Jmsg(jcr, M_INFO, 0, _("Purging oldest volume \"%s\"\n"), mr->VolumeName);
                  ok = purge_jobs_from_volume(ua, mr);
               /*
                * 8. or try recycling the oldest volume
                */
               } else if (jcr->pool->recycle_oldest_volume) {
                  Jmsg(jcr, M_INFO, 0, _("Pruning oldest volume \"%s\"\n"), mr->VolumeName);
                  ok = prune_volume(ua, mr);
               }
               free_ua_context(ua);
               if (ok) {
                  ok = recycle_volume(jcr, mr);
                  Dmsg1(400, "Recycle after purge oldest=%d\n", ok);
               }
            }
         }
      }
      Dmsg2(100, "VolJobs=%d FirstWritten=%d\n", mr->VolJobs, mr->FirstWritten);
      if (ok) {
         /* If we can use the volume, check if it is expired */
         if (has_volume_expired(jcr, mr)) {
            if (retry++ < 200) {            /* sanity check */
               continue;                    /* try again from the top */
            } else {
               Jmsg(jcr, M_ERROR, 0, _(
"We seem to be looping trying to find the next volume. I give up.\n"));
            }
         }
      }
      break;
   } /* end for loop */
   db_unlock(jcr->db);
   Dmsg1(150, "return ok=%d find_next_vol\n", ok);
   return ok;
}

/*
 * Check if any time limits or use limits have expired
 *   if so, set the VolStatus appropriately.
 */
bool has_volume_expired(JCR *jcr, MEDIA_DBR *mr)
{
   bool expired = false;
   /*
    * Check limits and expirations if "Append" and it has been used
    * i.e. mr->VolJobs > 0
    *
    */
   if (strcmp(mr->VolStatus, "Append") == 0 && mr->VolJobs > 0) {
      /* First handle Max Volume Bytes */
      if ((mr->MaxVolBytes > 0 && mr->VolBytes >= mr->MaxVolBytes)) {
         Jmsg(jcr, M_INFO, 0, _("Max Volume bytes exceeded. "
             "Marking Volume \"%s\" as Full.\n"), mr->VolumeName);
         bstrncpy(mr->VolStatus, "Full", sizeof(mr->VolStatus));
         expired = true;

      /* Now see if Volume should only be used once */
      } else if (mr->VolBytes > 0 && jcr->pool->use_volume_once) {
         Jmsg(jcr, M_INFO, 0, _("Volume used once. "
             "Marking Volume \"%s\" as Used.\n"), mr->VolumeName);
         bstrncpy(mr->VolStatus, "Used", sizeof(mr->VolStatus));
         expired = true;

      /* Now see if Max Jobs written to volume */
      } else if (mr->MaxVolJobs > 0 && mr->MaxVolJobs <= mr->VolJobs) {
         Jmsg(jcr, M_INFO, 0, _("Max Volume jobs exceeded. "
             "Marking Volume \"%s\" as Used.\n"), mr->VolumeName);
         bstrncpy(mr->VolStatus, "Used", sizeof(mr->VolStatus));
         expired = true;

      /* Now see if Max Files written to volume */
      } else if (mr->MaxVolFiles > 0 && mr->MaxVolFiles <= mr->VolFiles) {
         Jmsg(jcr, M_INFO, 0, _("Max Volume files exceeded. "
             "Marking Volume \"%s\" as Used.\n"), mr->VolumeName);
         bstrncpy(mr->VolStatus, "Used", sizeof(mr->VolStatus));
         expired = true;

      /* Finally, check Use duration expiration */
      } else if (mr->VolUseDuration > 0) {
         utime_t now = time(NULL);
         /* See if Vol Use has expired */
         if (mr->VolUseDuration <= (now - mr->FirstWritten)) {
            Jmsg(jcr, M_INFO, 0, _("Max configured use duration exceeded. "
               "Marking Volume \"%s\" as Used.\n"), mr->VolumeName);
            bstrncpy(mr->VolStatus, "Used", sizeof(mr->VolStatus));
            expired = true;
         }
      }
   }
   if (expired) {
      /* Need to update media */
      if (!db_update_media_record(jcr, jcr->db, mr)) {
         Jmsg(jcr, M_ERROR, 0, _("Catalog error updating volume \"%s\". ERR=%s"),
              mr->VolumeName, db_strerror(jcr->db));
      }
   }
   return expired;
}

/*
 * Try hard to recycle the current volume
 *
 *  Returns: on failure - reason = NULL
 *           on success - reason - pointer to reason
 */
void check_if_volume_valid_or_recyclable(JCR *jcr, MEDIA_DBR *mr, const char **reason)
{
   int ok;

   *reason = NULL;

   /*  Check if a duration or limit has expired */
   if (has_volume_expired(jcr, mr)) {
      *reason = _("volume has expired");
      /* Keep going because we may be able to recycle volume */
   }

   /*
    * Now see if we can use the volume as is
    */
   if (strcmp(mr->VolStatus, "Append") == 0 ||
       strcmp(mr->VolStatus, "Recycle") == 0) {
      *reason = NULL;
      return;
   }

   /*
    * Check if the Volume is already marked for recycling
    */
   if (strcmp(mr->VolStatus, "Purged") == 0) {
      if (recycle_volume(jcr, mr)) {
         Jmsg(jcr, M_INFO, 0, _("Recycled current volume \"%s\"\n"), mr->VolumeName);
         *reason = NULL;
         return;
      } else {
         /* In principle this shouldn't happen */
         *reason = _("and recycling of current volume failed");
         return;
      }
   }

   /* At this point, the volume is not valid for writing */
   *reason = _("but should be Append, Purged or Recycle");

   /*
    * What we're trying to do here is see if the current volume is
    * "recyclable" - ie. if we prune all expired jobs off it, is
    * it now possible to reuse it for the job that it is currently
    * needed for?
    */
   if ((mr->LastWritten + mr->VolRetention) < (utime_t)time(NULL)
         && mr->Recycle && jcr->pool->recycle_current_volume
         && (strcmp(mr->VolStatus, "Full") == 0 ||
            strcmp(mr->VolStatus, "Used") == 0)) {
      /*
       * Attempt prune of current volume to see if we can
       * recycle it for use.
       */
      UAContext *ua;

      ua = new_ua_context(jcr);
      ok = prune_volume(ua, mr);
      free_ua_context(ua);

      if (ok) {
         /* If fully purged, recycle current volume */
         if (recycle_volume(jcr, mr)) {
            Jmsg(jcr, M_INFO, 0, _("Recycled current volume \"%s\"\n"), mr->VolumeName);
            *reason = NULL;
         } else {
            *reason = _("but should be Append, Purged or Recycle (recycling of the "
               "current volume failed)");
         }
      } else {
         *reason = _("but should be Append, Purged or Recycle (cannot automatically "
            "recycle current volume, as it still contains unpruned data "
            "or the Volume Retention time has not expired.)");
      }
   }
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static bool get_scratch_volume(JCR *jcr, MEDIA_DBR *mr, bool InChanger)
{
   MEDIA_DBR smr;
   POOL_DBR spr, pr;
   bool ok = false;
   bool found = false;
   char ed1[50], ed2[50];

   /* Only one thread at a time can pull from the scratch pool */
   P(mutex);
   /* 
    * Get Pool record for Scratch Pool
    */
   memset(&spr, 0, sizeof(spr));
   bstrncpy(spr.Name, "Scratch", sizeof(spr.Name));
   if (db_get_pool_record(jcr, jcr->db, &spr)) {
      memset(&smr, 0, sizeof(smr));
      smr.PoolId = spr.PoolId;
      if (InChanger) {       
         smr.StorageId = mr->StorageId;  /* want only Scratch Volumes in changer */
      }
      bstrncpy(smr.VolStatus, "Append", sizeof(smr.VolStatus));  /* want only appendable volumes */
      bstrncpy(smr.MediaType, mr->MediaType, sizeof(smr.MediaType));

      /*
       * If we do not find a valid Scratch volume, try
       *  recycling any existing purged volumes, then
       *  try to take the oldest volume.
       */
      if (db_find_next_volume(jcr, jcr->db, 1, InChanger, &smr)) {
         found = true;

      } else if (find_recycled_volume(jcr, InChanger, &smr)) {
         found = true;

      } else if (recycle_oldest_purged_volume(jcr, InChanger, &smr)) {
         found = true;
      }

      if (found) {
         POOL_MEM query(PM_MESSAGE);

         /*   
          * Get pool record where the Scratch Volume will go to ensure
          * that we can add a Volume.
          */
         memset(&pr, 0, sizeof(pr));
         bstrncpy(pr.Name, jcr->pool->hdr.name, sizeof(pr.Name));
         if (!db_get_pool_record(jcr, jcr->db, &pr)) {
            Jmsg(jcr, M_WARNING, 0, _("Unable to get Pool record: ERR=%s"), 
                 db_strerror(jcr->db));
            goto bail_out;
         }
         if (pr.MaxVols > 0 && pr.NumVols >= pr.MaxVols) {
            Jmsg(jcr, M_WARNING, 0, _("Unable add Scratch Volume, Pool \"%s\" full MaxVols=%d\n"),
               jcr->pool->hdr.name, pr.MaxVols);
            goto bail_out;
         }

         /* OK, now move Scratch Volume */
         db_lock(jcr->db);
         Mmsg(query, "UPDATE Media SET PoolId=%s WHERE MediaId=%s",
              edit_int64(mr->PoolId, ed1),
              edit_int64(smr.MediaId, ed2));
         ok = db_sql_query(jcr->db, query.c_str(), NULL, NULL);  
         db_unlock(jcr->db);
         if (!ok) {
            Jmsg(jcr, M_WARNING, 0, _("Failed to move Scratch Volume. ERR=%s\n"),
               db_strerror(jcr->db));
           goto bail_out;
          }
         Jmsg(jcr, M_INFO, 0, _("Using Volume \"%s\" from 'Scratch' pool.\n"), 
              smr.VolumeName);
         /* Set new Pool Id in smr record, then copy it to mr */
         smr.PoolId = mr->PoolId;
         memcpy(mr, &smr, sizeof(MEDIA_DBR));
         /* Set default parameters from current pool */
         set_pool_dbr_defaults_in_media_dbr(mr, &pr);
         /* set_pool_dbr_defaults_in_media_dbr set VolStatus to Append,
          * we could have Recycled media */
         bstrncpy(mr->VolStatus, smr.VolStatus, sizeof(smr.VolStatus));
         if (!db_update_media_record(jcr, jcr->db, mr)) {
            Jmsg(jcr, M_WARNING, 0, _("Unable to update Volume record: ERR=%s"), 
                 db_strerror(jcr->db));
            ok = false;
         }
      }
   }
bail_out:
   V(mutex);
   return ok;
}
