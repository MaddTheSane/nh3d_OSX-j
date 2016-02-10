/* NetHack 3.6	polyself.c	$NHDT-Date: 1448496566 2015/11/26 00:09:26 $  $NHDT-Branch: master $:$NHDT-Revision: 1.104 $ */
/*      Copyright (C) 1987, 1988, 1989 by Ken Arromdee */
/* NetHack may be freely redistributed.  See license for details. */

/*
**	Japanese version Copyright
**	(c) Issei Numata, Naoki Hamada, Shigehiro Miyashita, 1994-2000
**	For 3.4, Copyright (c) Kentaro Shirakata, 2002-2003
**	JNetHack may be freely redistributed.  See license for details. 
*/

/*
 * Polymorph self routine.
 *
 * Note:  the light source handling code assumes that both youmonst.m_id
 * and youmonst.mx will always remain 0 when it handles the case of the
 * player polymorphed into a light-emitting monster.
 *
 * Transformation sequences:
 *              /-> polymon                 poly into monster form
 *    polyself =
 *              \-> newman -> polyman       fail to poly, get human form
 *
 *    rehumanize -> polyman                 return to original form
 *
 *    polymon (called directly)             usually golem petrification
 */

#include "hack.h"

STATIC_DCL void FDECL(check_strangling, (BOOLEAN_P));
STATIC_DCL void FDECL(polyman, (const char *, const char *));
STATIC_DCL void NDECL(break_armor);
STATIC_DCL void FDECL(drop_weapon, (int));
STATIC_DCL void NDECL(uunstick);
STATIC_DCL int FDECL(armor_to_dragon, (int));
STATIC_DCL void NDECL(newman);
STATIC_DCL boolean FDECL(polysense, (struct permonst *));

STATIC_VAR const char no_longer_petrify_resistant[] =
/*JP
    "No longer petrify-resistant, you";
*/
    "�в��ؤ����Ϥ��ʤ��ʤäơ����ʤ���";

/* controls whether taking on new form or becoming new man can also
   change sex (ought to be an arg to polymon() and newman() instead) */
STATIC_VAR int sex_change_ok = 0;

/* update the youmonst.data structure pointer and intrinsics */
void
set_uasmon()
{
    struct permonst *mdat = &mons[u.umonnum];

    set_mon_data(&youmonst, mdat, 0);

#define PROPSET(PropIndx, ON)                          \
    do {                                               \
        if (ON)                                        \
            u.uprops[PropIndx].intrinsic |= FROMFORM;  \
        else                                           \
            u.uprops[PropIndx].intrinsic &= ~FROMFORM; \
    } while (0)

    PROPSET(FIRE_RES, resists_fire(&youmonst));
    PROPSET(COLD_RES, resists_cold(&youmonst));
    PROPSET(SLEEP_RES, resists_sleep(&youmonst));
    PROPSET(DISINT_RES, resists_disint(&youmonst));
    PROPSET(SHOCK_RES, resists_elec(&youmonst));
    PROPSET(POISON_RES, resists_poison(&youmonst));
    PROPSET(ACID_RES, resists_acid(&youmonst));
    PROPSET(STONE_RES, resists_ston(&youmonst));
    {
        /* resists_drli() takes wielded weapon into account; suppress it */
        struct obj *save_uwep = uwep;

        uwep = 0;
        PROPSET(DRAIN_RES, resists_drli(&youmonst));
        uwep = save_uwep;
    }
    /* resists_magm() takes wielded, worn, and carried equipment into
       into account; cheat and duplicate its monster-specific part */
    PROPSET(ANTIMAGIC, (dmgtype(mdat, AD_MAGM)
                        || mdat == &mons[PM_BABY_GRAY_DRAGON]
                        || dmgtype(mdat, AD_RBRE)));
    PROPSET(SICK_RES, (mdat->mlet == S_FUNGUS || mdat == &mons[PM_GHOUL]));

    PROPSET(STUNNED, (mdat == &mons[PM_STALKER] || is_bat(mdat)));
    PROPSET(HALLUC_RES, dmgtype(mdat, AD_HALU));
    PROPSET(SEE_INVIS, perceives(mdat));
    PROPSET(TELEPAT, telepathic(mdat));
    PROPSET(INFRAVISION, infravision(mdat));
    PROPSET(INVIS, pm_invisible(mdat));
    PROPSET(TELEPORT, can_teleport(mdat));
    PROPSET(TELEPORT_CONTROL, control_teleport(mdat));
    PROPSET(LEVITATION, is_floater(mdat));
    PROPSET(FLYING, is_flyer(mdat));
    PROPSET(SWIMMING, is_swimmer(mdat));
    /* [don't touch MAGICAL_BREATHING here; both Amphibious and Breathless
       key off of it but include different monster forms...] */
    PROPSET(PASSES_WALLS, passes_walls(mdat));
    PROPSET(REGENERATION, regenerates(mdat));
    PROPSET(REFLECTING, (mdat == &mons[PM_SILVER_DRAGON]));

    float_vs_flight(); /* maybe toggle (BFlying & I_SPECIAL) */

#undef PROPSET

#ifdef STATUS_VIA_WINDOWPORT
    status_initialize(REASSESS_ONLY);
#endif
}

/* Levitation overrides Flying; set or clear BFlying|I_SPECIAL */
void
float_vs_flight()
{
    /* floating overrides flight; normally float_up() and float_down()
       handle this, but sometimes they're skipped */
    if (HLevitation || ELevitation)
        BFlying |= I_SPECIAL;
    else
        BFlying &= ~I_SPECIAL;
}

/* for changing into form that's immune to strangulation */
STATIC_OVL void
check_strangling(on)
boolean on;
{
    /* on -- maybe resume strangling */
    if (on) {
        /* when Strangled is already set, polymorphing from one
           vulnerable form into another causes the counter to be reset */
        if (uamul && uamul->otyp == AMULET_OF_STRANGULATION
            && can_be_strangled(&youmonst)) {
            Your("%s %s your %s!", simpleonames(uamul),
                 Strangled ? "still constricts" : "begins constricting",
                 body_part(NECK)); /* "throat" */
            Strangled = 6L;
            makeknown(AMULET_OF_STRANGULATION);
        }

    /* off -- maybe block strangling */
    } else {
        if (Strangled && !can_be_strangled(&youmonst)) {
            Strangled = 0L;
            You("are no longer being strangled.");
        }
    }
}

/* make a (new) human out of the player */
STATIC_OVL void
polyman(fmt, arg)
const char *fmt, *arg;
{
    boolean sticky = (sticks(youmonst.data) && u.ustuck && !u.uswallow),
            was_mimicking = (youmonst.m_ap_type == M_AP_OBJECT);
    boolean was_blind = !!Blind;

    if (Upolyd) {
        u.acurr = u.macurr; /* restore old attribs */
        u.amax = u.mamax;
        u.umonnum = u.umonster;
        flags.female = u.mfemale;
    }
    set_uasmon();

    u.mh = u.mhmax = 0;
    u.mtimedone = 0;
    skinback(FALSE);
    u.uundetected = 0;

    if (sticky)
        uunstick();
    find_ac();
    if (was_mimicking) {
        if (multi < 0)
            unmul("");
        youmonst.m_ap_type = M_AP_NOTHING;
    }

    newsym(u.ux, u.uy);

    You(fmt, arg);
    /* check whether player foolishly genocided self while poly'd */
    if ((mvitals[urole.malenum].mvflags & G_GENOD)
        || (urole.femalenum != NON_PM
            && (mvitals[urole.femalenum].mvflags & G_GENOD))
        || (mvitals[urace.malenum].mvflags & G_GENOD)
        || (urace.femalenum != NON_PM
            && (mvitals[urace.femalenum].mvflags & G_GENOD))) {
        /* intervening activity might have clobbered genocide info */
        struct kinfo *kptr = find_delayed_killer(POLYMORPH);

        if (kptr != (struct kinfo *) 0 && kptr->name[0]) {
            killer.format = kptr->format;
            Strcpy(killer.name, kptr->name);
        } else {
            killer.format = KILLED_BY;
/*JP
            Strcpy(killer.name, "self-genocide");
*/
            Strcpy(killer.name, "����Ū�Ի���");
        }
        dealloc_killer(kptr);
        done(GENOCIDED);
    }

    if (u.twoweap && !could_twoweap(youmonst.data))
        untwoweapon();

    if (u.utraptype == TT_PIT && u.utrap) {
        u.utrap = rn1(6, 2); /* time to escape resets */
    }
    if (was_blind && !Blind) { /* reverting from eyeless */
        Blinded = 1L;
        make_blinded(0L, TRUE); /* remove blindness */
    }
    check_strangling(TRUE);

    if (!Levitation && !u.ustuck && is_pool_or_lava(u.ux, u.uy))
        spoteffects(TRUE);

    see_monsters();
}

void
change_sex()
{
    /* setting u.umonster for caveman/cavewoman or priest/priestess
       swap unintentionally makes `Upolyd' appear to be true */
    boolean already_polyd = (boolean) Upolyd;

    /* Some monsters are always of one sex and their sex can't be changed;
     * Succubi/incubi can change, but are handled below.
     *
     * !already_polyd check necessary because is_male() and is_female()
     * are true if the player is a priest/priestess.
     */
    if (!already_polyd
        || (!is_male(youmonst.data) && !is_female(youmonst.data)
            && !is_neuter(youmonst.data)))
        flags.female = !flags.female;
    if (already_polyd) /* poly'd: also change saved sex */
        u.mfemale = !u.mfemale;
    max_rank_sz(); /* [this appears to be superfluous] */
    if ((already_polyd ? u.mfemale : flags.female) && urole.name.f)
        Strcpy(pl_character, urole.name.f);
    else
        Strcpy(pl_character, urole.name.m);
    u.umonster = ((already_polyd ? u.mfemale : flags.female)
                  && urole.femalenum != NON_PM)
                     ? urole.femalenum
                     : urole.malenum;
    if (!already_polyd) {
        u.umonnum = u.umonster;
    } else if (u.umonnum == PM_SUCCUBUS || u.umonnum == PM_INCUBUS) {
        flags.female = !flags.female;
        /* change monster type to match new sex */
        u.umonnum = (u.umonnum == PM_SUCCUBUS) ? PM_INCUBUS : PM_SUCCUBUS;
        set_uasmon();
    }
}

STATIC_OVL void
newman()
{
    int i, oldlvl, newlvl, hpmax, enmax;

    oldlvl = u.ulevel;
    newlvl = oldlvl + rn1(5, -2);     /* new = old + {-2,-1,0,+1,+2} */
    if (newlvl > 127 || newlvl < 1) { /* level went below 0? */
        goto dead; /* old level is still intact (in case of lifesaving) */
    }
    if (newlvl > MAXULEV)
        newlvl = MAXULEV;
    /* If your level goes down, your peak level goes down by
       the same amount so that you can't simply use blessed
       full healing to undo the decrease.  But if your level
       goes up, your peak level does *not* undergo the same
       adjustment; you might end up losing out on the chance
       to regain some levels previously lost to other causes. */
    if (newlvl < oldlvl)
        u.ulevelmax -= (oldlvl - newlvl);
    if (u.ulevelmax < newlvl)
        u.ulevelmax = newlvl;
    u.ulevel = newlvl;

    if (sex_change_ok && !rn2(10))
        change_sex();

    adjabil(oldlvl, (int) u.ulevel);
    reset_rndmonst(NON_PM); /* new monster generation criteria */

    /* random experience points for the new experience level */
    u.uexp = rndexp(FALSE);

    /* set up new attribute points (particularly Con) */
    redist_attr();

    /*
     * New hit points:
     *  remove level-gain based HP from any extra HP accumulated
     *  (the "extra" might actually be negative);
     *  modify the extra, retaining {80%, 90%, 100%, or 110%};
     *  add in newly generated set of level-gain HP.
     *
     * (This used to calculate new HP in direct proportion to old HP,
     * but that was subject to abuse:  accumulate a large amount of
     * extra HP, drain level down to 1, then polyself to level 2 or 3
     * [lifesaving capability needed to handle level 0 and -1 cases]
     * and the extra got multiplied by 2 or 3.  Repeat the level
     * drain and polyself steps until out of lifesaving capability.)
     */
    hpmax = u.uhpmax;
    for (i = 0; i < oldlvl; i++)
        hpmax -= (int) u.uhpinc[i];
    /* hpmax * rn1(4,8) / 10; 0.95*hpmax on average */
    hpmax = rounddiv((long) hpmax * (long) rn1(4, 8), 10);
    for (i = 0; (u.ulevel = i) < newlvl; i++)
        hpmax += newhp();
    if (hpmax < u.ulevel)
        hpmax = u.ulevel; /* min of 1 HP per level */
    /* retain same proportion for current HP; u.uhp * hpmax / u.uhpmax */
    u.uhp = rounddiv((long) u.uhp * (long) hpmax, u.uhpmax);
    u.uhpmax = hpmax;
    /*
     * Do the same for spell power.
     */
    enmax = u.uenmax;
    for (i = 0; i < oldlvl; i++)
        enmax -= (int) u.ueninc[i];
    enmax = rounddiv((long) enmax * (long) rn1(4, 8), 10);
    for (i = 0; (u.ulevel = i) < newlvl; i++)
        enmax += newpw();
    if (enmax < u.ulevel)
        enmax = u.ulevel;
    u.uen = rounddiv((long) u.uen * (long) enmax,
                     ((u.uenmax < 1) ? 1 : u.uenmax));
    u.uenmax = enmax;
    /* [should alignment record be tweaked too?] */

    u.uhunger = rn1(500, 500);
    if (Sick)
        make_sick(0L, (char *) 0, FALSE, SICK_ALL);
    if (Stoned)
        make_stoned(0L, (char *) 0, 0, (char *) 0);
    if (u.uhp <= 0) {
        if (Polymorph_control) { /* even when Stunned || Unaware */
            if (u.uhp <= 0)
                u.uhp = 1;
        } else {
        dead: /* we come directly here if their experience level went to 0 or
                 less */
/*JP
            Your("new form doesn't seem healthy enough to survive.");
*/
            Your("�������Ѥ������Ƥ����������Ϥ��ʤ��褦����");
            killer.format = KILLED_BY_AN;
/*JP
            Strcpy(killer.name, "unsuccessful polymorph");
*/
            Strcpy(killer.name, "�Ѳ��μ��Ԥ�");
            done(DIED);
            newuhs(FALSE);
            (void) polysense(youmonst.data);
            return; /* lifesaved */
        }
    }
    newuhs(FALSE);
/*JP
    polyman("feel like a new %s!",
*/
    polyman("%s�Ȥ������ޤ줫��ä��褦�ʵ���������",
            /* use saved gender we're about to revert to, not current */
            (u.mfemale && urace.individual.f)
                ? urace.individual.f
                : (urace.individual.m) ? urace.individual.m : urace.noun);
    if (Slimed) {
/*JP
        Your("body transforms, but there is still slime on you.");
*/
        Your("�Τ��Ѳ������������饤�ब�Ĥ����ޤޤ���");
        make_slimed(10L, (const char *) 0);
    }

    (void) polysense(youmonst.data);
    context.botl = 1;
    see_monsters();
    (void) encumber_msg();

    retouch_equipment(2);
    if (!uarmg)
        selftouch(no_longer_petrify_resistant);
}

void
polyself(psflags)
int psflags;
{
    char buf[BUFSZ];
    int old_light, new_light, mntmp, class, tryct;
    boolean forcecontrol = (psflags == 1), monsterpoly = (psflags == 2),
            draconian = (uarm && Is_dragon_armor(uarm)),
            iswere = (u.ulycn >= LOW_PM), isvamp = is_vampire(youmonst.data),
            controllable_poly = Polymorph_control && !(Stunned || Unaware);

    if (Unchanging) {
        pline("You fail to transform!");
        return;
    }
    /* being Stunned|Unaware doesn't negate this aspect of Poly_control */
    if (!Polymorph_control && !forcecontrol && !draconian && !iswere
        && !isvamp) {
        if (rn2(20) > ACURR(A_CON)) {
            You1(shudder_for_moment);
/*JP
            losehp(rnd(30), "system shock", KILLED_BY_AN);
*/
            losehp(rnd(30), "�����ƥॷ��å���", KILLED_BY_AN);
            exercise(A_CON, FALSE);
            return;
        }
    }
    old_light = emits_light(youmonst.data);
    mntmp = NON_PM;

    if (monsterpoly && isvamp)
        goto do_vampyr;

    if (controllable_poly || forcecontrol) {
        tryct = 5;
        do {
            mntmp = NON_PM;
/*JP
            getlin("Become what kind of monster? [type the name]", buf);
*/
            getlin("�ɤμ�β�ʪ�ˤʤ롩[̾��������Ƥ�]", buf);
            (void) mungspaces(buf);
            if (*buf == '\033') {
                /* user is cancelling controlled poly */
                if (forcecontrol) { /* wizard mode #polyself */
                    pline1(Never_mind);
                    return;
                }
                Strcpy(buf, "*"); /* resort to random */
            }
            if (!strcmp(buf, "*") || !strcmp(buf, "random")) {
                /* explicitly requesting random result */
                tryct = 0; /* will skip thats_enough_tries */
                continue;  /* end do-while(--tryct > 0) loop */
            }
            class = 0;
            mntmp = name_to_mon(buf);
            if (mntmp < LOW_PM) {
            by_class:
                class = name_to_monclass(buf, &mntmp);
                if (class && mntmp == NON_PM)
                    mntmp = mkclass_poly(class);
            }
            if (mntmp < LOW_PM) {
                if (!class)
/*JP
                    pline("I've never heard of such monsters.");
*/
                    pline("����ʲ�ʪ��ʹ�������Ȥ��ʤ���");
                else
/*JP
                    You_cant("polymorph into any of those.");
*/
                    pline("����ˤʤ뤳�ȤϤǤ��ʤ���");
            } else if (iswere && (were_beastie(mntmp) == u.ulycn
                                  || mntmp == counter_were(u.ulycn)
                                  || (Upolyd && mntmp == PM_HUMAN))) {
                goto do_shift;
                /* Note:  humans are illegal as monsters, but an
                 * illegal monster forces newman(), which is what we
                 * want if they specified a human.... */
            } else if (!polyok(&mons[mntmp])
                       && !(mntmp == PM_HUMAN || your_race(&mons[mntmp])
                            || mntmp == urole.malenum
                            || mntmp == urole.femalenum)) {
                const char *pm_name;

                /* mkclass_poly() can pick a !polyok()
                   candidate; if so, usually try again */
                if (class) {
                    if (rn2(3) || --tryct > 0)
                        goto by_class;
                    /* no retries left; put one back on counter
                       so that end of loop decrement will yield
                       0 and trigger thats_enough_tries message */
                    ++tryct;
                }
                pm_name = mons[mntmp].mname;
                if (the_unique_pm(&mons[mntmp]))
                    pm_name = the(pm_name);
                else if (!type_is_pname(&mons[mntmp]))
                    pm_name = an(pm_name);
                You_cant("polymorph into %s.", pm_name);
            } else
                break;
        } while (--tryct > 0);
        if (!tryct)
            pline1(thats_enough_tries);
        /* allow skin merging, even when polymorph is controlled */
        if (draconian && (tryct <= 0 || mntmp == armor_to_dragon(uarm->otyp)))
            goto do_merge;
        if (isvamp && (tryct <= 0 || mntmp == PM_WOLF || mntmp == PM_FOG_CLOUD
                       || is_bat(&mons[mntmp])))
            goto do_vampyr;
    } else if (draconian || iswere || isvamp) {
        /* special changes that don't require polyok() */
        if (draconian) {
        do_merge:
            mntmp = armor_to_dragon(uarm->otyp);
            if (!(mvitals[mntmp].mvflags & G_GENOD)) {
                /* allow G_EXTINCT */
                if (Is_dragon_scales(uarm)) {
                    /* dragon scales remain intact as uskin */
/*JP
                    You("merge with your scaly armor.");
*/
                    You("�ڤγ��Ȱ��β�������");
                } else { /* dragon scale mail */
                    /* d.scale mail first reverts to scales */
                    char *p, *dsmail;

                    /* similar to noarmor(invent.c),
                       shorten to "<color> scale mail" */
                    dsmail = strcpy(buf, simpleonames(uarm));
                    if ((p = strstri(dsmail, " dragon ")) != 0)
                        while ((p[1] = p[8]) != '\0')
                            ++p;
                    /* tricky phrasing; dragon scale mail
                       is singular, dragon scales are plural */
                    Your("%s reverts to scales as you merge with them.",
                         dsmail);
                    /* uarm->spe enchantment remains unchanged;
                       re-converting scales to mail poses risk
                       of evaporation due to over enchanting */
                    uarm->otyp += GRAY_DRAGON_SCALES - GRAY_DRAGON_SCALE_MAIL;
                    uarm->dknown = 1;
                    context.botl = 1; /* AC is changing */
                }
                uskin = uarm;
                uarm = (struct obj *) 0;
                /* save/restore hack */
                uskin->owornmask |= I_SPECIAL;
                update_inventory();
            }
        } else if (iswere) {
        do_shift:
            if (Upolyd && were_beastie(mntmp) != u.ulycn)
                mntmp = PM_HUMAN; /* Illegal; force newman() */
            else
                mntmp = u.ulycn;
        } else if (isvamp) {
        do_vampyr:
            if (mntmp < LOW_PM || (mons[mntmp].geno & G_UNIQ))
                mntmp = (youmonst.data != &mons[PM_VAMPIRE] && !rn2(10))
                            ? PM_WOLF
                            : !rn2(4) ? PM_FOG_CLOUD : PM_VAMPIRE_BAT;
            if (controllable_poly) {
                Sprintf(buf, "Become %s?", an(mons[mntmp].mname));
                if (yn(buf) != 'y')
                    return;
            }
        }
        /* if polymon fails, "you feel" message has been given
           so don't follow up with another polymon or newman;
           sex_change_ok left disabled here */
        if (mntmp == PM_HUMAN)
            newman(); /* werecritter */
        else
            (void) polymon(mntmp);
        goto made_change; /* maybe not, but this is right anyway */
    }

    if (mntmp < LOW_PM) {
        tryct = 200;
        do {
            /* randomly pick an "ordinary" monster */
            mntmp = rn1(SPECIAL_PM - LOW_PM, LOW_PM);
            if (polyok(&mons[mntmp]) && !is_placeholder(&mons[mntmp]))
                break;
        } while (--tryct > 0);
    }

    /* The below polyok() fails either if everything is genocided, or if
     * we deliberately chose something illegal to force newman().
     */
    sex_change_ok++;
    if (!polyok(&mons[mntmp]) || (!forcecontrol && !rn2(5))
        || your_race(&mons[mntmp])) {
        newman();
    } else {
        (void) polymon(mntmp);
    }
    sex_change_ok--; /* reset */

made_change:
    new_light = emits_light(youmonst.data);
    if (old_light != new_light) {
        if (old_light)
            del_light_source(LS_MONSTER, monst_to_any(&youmonst));
        if (new_light == 1)
            ++new_light; /* otherwise it's undetectable */
        if (new_light)
            new_light_source(u.ux, u.uy, new_light, LS_MONSTER,
                             monst_to_any(&youmonst));
    }
}

/* (try to) make a mntmp monster out of the player;
   returns 1 if polymorph successful */
int
polymon(mntmp)
int mntmp;
{
    boolean sticky = sticks(youmonst.data) && u.ustuck && !u.uswallow,
            was_blind = !!Blind, dochange = FALSE;
    int mlvl;

    if (mvitals[mntmp].mvflags & G_GENOD) { /* allow G_EXTINCT */
/*JP
        You_feel("rather %s-ish.", mons[mntmp].mname);
*/
        You("%s�äݤ��ʤä��褦�ʵ���������", mons[mntmp].mname);
        exercise(A_WIS, TRUE);
        return 0;
    }

    /* KMH, conduct */
    u.uconduct.polyselfs++;

    /* exercise used to be at the very end but only Wis was affected
       there since the polymorph was always in effect by then */
    exercise(A_CON, FALSE);
    exercise(A_WIS, TRUE);

    if (!Upolyd) {
        /* Human to monster; save human stats */
        u.macurr = u.acurr;
        u.mamax = u.amax;
        u.mfemale = flags.female;
    } else {
        /* Monster to monster; restore human stats, to be
         * immediately changed to provide stats for the new monster
         */
        u.acurr = u.macurr;
        u.amax = u.mamax;
        flags.female = u.mfemale;
    }

    /* if stuck mimicking gold, stop immediately */
    if (multi < 0 && youmonst.m_ap_type == M_AP_OBJECT
        && youmonst.data->mlet != S_MIMIC)
        unmul("");
    /* if becoming a non-mimic, stop mimicking anything */
    if (mons[mntmp].mlet != S_MIMIC) {
        /* as in polyman() */
        youmonst.m_ap_type = M_AP_NOTHING;
    }
    if (is_male(&mons[mntmp])) {
        if (flags.female)
            dochange = TRUE;
    } else if (is_female(&mons[mntmp])) {
        if (!flags.female)
            dochange = TRUE;
    } else if (!is_neuter(&mons[mntmp]) && mntmp != u.ulycn) {
        if (sex_change_ok && !rn2(10))
            dochange = TRUE;
    }
    if (dochange) {
        flags.female = !flags.female;
#if 0 /*JP*/
        You("%s %s%s!",
            (u.umonnum != mntmp) ? "turn into a" : "feel like a new",
            (is_male(&mons[mntmp]) || is_female(&mons[mntmp]))
                ? ""
                : flags.female ? "female " : "male ",
            mons[mntmp].mname);
#else
        You("%s%s�ˤʤä�%s��",
            (is_male(&mons[mntmp]) || is_female(&mons[mntmp]))
                ? ""
                : flags.female ? "����" : "�ˤ�",
            mons[mntmp].mname,
            (u.umonnum != mntmp) ? "" : "�褦�ʵ�������");
#endif
    } else {
        if (u.umonnum != mntmp)
/*JP
            You("turn into %s!", an(mons[mntmp].mname));
*/
            You("%s�ˤʤä���", mons[mntmp].mname);
        else
/*JP
            You_feel("like a new %s!", mons[mntmp].mname);
*/
            You("�̤�%s�ˤʤä��褦�ʵ���������", mons[mntmp].mname);
    }
    if (Stoned && poly_when_stoned(&mons[mntmp])) {
        /* poly_when_stoned already checked stone golem genocide */
        mntmp = PM_STONE_GOLEM;
/*JP
        make_stoned(0L, "You turn to stone!", 0, (char *) 0);
*/
        make_stoned(0L, "�Фˤʤä���", 0, (char *) 0);
    }

    u.mtimedone = rn1(500, 500);
    u.umonnum = mntmp;
    set_uasmon();

    /* New stats for monster, to last only as long as polymorphed.
     * Currently only strength gets changed.
     */
    if (strongmonst(&mons[mntmp]))
        ABASE(A_STR) = AMAX(A_STR) = STR18(100);

    if (Stone_resistance && Stoned) { /* parnes@eniac.seas.upenn.edu */
/*JP
        make_stoned(0L, "You no longer seem to be petrifying.", 0,
*/
        make_stoned(0L, "�в�����������줿�褦����", 0,
                    (char *) 0);
    }
    if (Sick_resistance && Sick) {
        make_sick(0L, (char *) 0, FALSE, SICK_ALL);
/*JP
        You("no longer feel sick.");
*/
        You("�µ�����������줿�褦����");
    }
    if (Slimed) {
        if (flaming(youmonst.data)) {
/*JP
            make_slimed(0L, "The slime burns away!");
*/
            make_slimed(0L, "���饤���ǳ������");
        } else if (mntmp == PM_GREEN_SLIME) {
            /* do it silently */
            make_slimed(0L, (char *) 0);
        }
    }
    check_strangling(FALSE); /* maybe stop strangling */
    if (nohands(youmonst.data))
        Glib = 0;

    /*
    mlvl = adj_lev(&mons[mntmp]);
     * We can't do the above, since there's no such thing as an
     * "experience level of you as a monster" for a polymorphed character.
     */
    mlvl = (int) mons[mntmp].mlevel;
    if (youmonst.data->mlet == S_DRAGON && mntmp >= PM_GRAY_DRAGON) {
        u.mhmax = In_endgame(&u.uz) ? (8 * mlvl) : (4 * mlvl + d(mlvl, 4));
    } else if (is_golem(youmonst.data)) {
        u.mhmax = golemhp(mntmp);
    } else {
        if (!mlvl)
            u.mhmax = rnd(4);
        else
            u.mhmax = d(mlvl, 8);
        if (is_home_elemental(&mons[mntmp]))
            u.mhmax *= 3;
    }
    u.mh = u.mhmax;

    if (u.ulevel < mlvl) {
        /* Low level characters can't become high level monsters for long */
#ifdef DUMB
        /* DRS/NS 2.2.6 messes up -- Peter Kendell */
        int mtd = u.mtimedone, ulv = u.ulevel;

        u.mtimedone = mtd * ulv / mlvl;
#else
        u.mtimedone = u.mtimedone * u.ulevel / mlvl;
#endif
    }

    if (uskin && mntmp != armor_to_dragon(uskin->otyp))
        skinback(FALSE);
    break_armor();
    drop_weapon(1);
    (void) hideunder(&youmonst);

    if (u.utraptype == TT_PIT && u.utrap) {
        u.utrap = rn1(6, 2); /* time to escape resets */
    }
    if (was_blind && !Blind) { /* previous form was eyeless */
        Blinded = 1L;
        make_blinded(0L, TRUE); /* remove blindness */
    }
    newsym(u.ux, u.uy); /* Change symbol */

    if (!sticky && !u.uswallow && u.ustuck && sticks(youmonst.data))
        u.ustuck = 0;
    else if (sticky && !sticks(youmonst.data))
        uunstick();
    if (u.usteed) {
        if (touch_petrifies(u.usteed->data) && !Stone_resistance && rnl(3)) {
            char buf[BUFSZ];

            pline("%s touch %s.", no_longer_petrify_resistant,
                  mon_nam(u.usteed));
            Sprintf(buf, "riding %s", an(u.usteed->data->mname));
            instapetrify(buf);
        }
        if (!can_ride(u.usteed))
            dismount_steed(DISMOUNT_POLY);
    }

    if (flags.verbose) {
/*JP
        static const char use_thec[] = "Use the command #%s to %s.";
*/
        static const char use_thec[] = "#%s���ޥ�ɤ�%s���Ȥ��Ǥ��롥";
        static const char monsterc[] = "monster";

        if (can_breathe(youmonst.data))
/*JP
            pline(use_thec, monsterc, "use your breath weapon");
*/
            pline(use_thec,monsterc, "©���Ǥ�������");
        if (attacktype(youmonst.data, AT_SPIT))
/*JP
            pline(use_thec, monsterc, "spit venom");
*/
            pline(use_thec,monsterc, "�Ǥ��Ǥ�");
        if (youmonst.data->mlet == S_NYMPH)
/*JP
            pline(use_thec, monsterc, "remove an iron ball");
*/
            pline(use_thec,monsterc, "Ŵ���Ϥ���");
        if (attacktype(youmonst.data, AT_GAZE))
/*JP
            pline(use_thec, monsterc, "gaze at monsters");
*/
            pline(use_thec,monsterc, "��ʪ���ˤ�");
        if (is_hider(youmonst.data))
/*JP
            pline(use_thec, monsterc, "hide");
*/
            pline(use_thec,monsterc, "�����");
        if (is_were(youmonst.data))
/*JP
            pline(use_thec, monsterc, "summon help");
*/
            pline(use_thec,monsterc, "��֤򾤴�����");
        if (webmaker(youmonst.data))
/*JP
            pline(use_thec, monsterc, "spin a web");
*/
            pline(use_thec,monsterc, "��������ĥ��");
        if (u.umonnum == PM_GREMLIN)
/*JP
            pline(use_thec, monsterc, "multiply in a fountain");
*/
            pline(use_thec,monsterc, "�������ʬ������");
        if (is_unicorn(youmonst.data))
/*JP
            pline(use_thec, monsterc, "use your horn");
*/
            pline(use_thec,monsterc, "�Ѥ�Ȥ�");
        if (is_mind_flayer(youmonst.data))
/*JP
            pline(use_thec, monsterc, "emit a mental blast");
*/
            pline(use_thec,monsterc, "�����Ȥ�ȯ��������");
        if (youmonst.data->msound == MS_SHRIEK) /* worthless, actually */
/*JP
            pline(use_thec, monsterc, "shriek");
*/
            pline(use_thec,monsterc, "���ڤ����򤢤���");
        if (is_vampire(youmonst.data))
/*JP
            pline(use_thec, monsterc, "change shape");
*/
            pline(use_thec, monsterc, "�Ѥ��Ѥ���");

        if (lays_eggs(youmonst.data) && flags.female)
/*JP
            pline(use_thec, "sit", "lay an egg");
*/
            pline(use_thec, "sit", "��򻺤�");
    }

    /* you now know what an egg of your type looks like */
    if (lays_eggs(youmonst.data)) {
        learn_egg_type(u.umonnum);
        /* make queen bees recognize killer bee eggs */
        learn_egg_type(egg_type_from_parent(u.umonnum, TRUE));
    }
    find_ac();
    if ((!Levitation && !u.ustuck && !Flying && is_pool_or_lava(u.ux, u.uy))
        || (Underwater && !Swimming))
        spoteffects(TRUE);
    if (Passes_walls && u.utrap
        && (u.utraptype == TT_INFLOOR || u.utraptype == TT_BURIEDBALL)) {
        u.utrap = 0;
        if (u.utraptype == TT_INFLOOR)
/*JP
            pline_The("rock seems to no longer trap you.");
*/
            pline("����Ĥ�������뤳�ȤϤʤ�������");
        else {
/*JP
            pline_The("buried ball is no longer bound to you.");
*/
            pline_The("��ޤä��夬����ˤʤ뤳�ȤϤʤ�������");
            buried_ball_to_freedom();
        }
    } else if (likes_lava(youmonst.data) && u.utrap
               && u.utraptype == TT_LAVA) {
        u.utrap = 0;
/*JP
        pline_The("lava now feels soothing.");
*/
        pline("�ϴ䤬����������Ĥ����Ƥ���롥");
    }
    if (amorphous(youmonst.data) || is_whirly(youmonst.data)
        || unsolid(youmonst.data)) {
        if (Punished) {
/*JP
            You("slip out of the iron chain.");
*/
            You("Ŵ�κ����餹����ȴ������");
            unpunish();
        } else if (u.utrap && u.utraptype == TT_BURIEDBALL) {
/*JP
            You("slip free of the buried ball and chain.");
*/
            You("��ޤäƤ����Ⱥ����餹����ȴ������");
            buried_ball_to_freedom();
        }
    }
    if (u.utrap && (u.utraptype == TT_WEB || u.utraptype == TT_BEARTRAP)
        && (amorphous(youmonst.data) || is_whirly(youmonst.data)
            || unsolid(youmonst.data) || (youmonst.data->msize <= MZ_SMALL
                                          && u.utraptype == TT_BEARTRAP))) {
#if 0 /*JP*/
        You("are no longer stuck in the %s.",
            u.utraptype == TT_WEB ? "web" : "bear trap");
#else
        You("%s����æ�Ф�����",
            u.utraptype == TT_WEB ? "�������" : "�����");
#endif
        /* probably should burn webs too if PM_FIRE_ELEMENTAL */
        u.utrap = 0;
    }
    if (webmaker(youmonst.data) && u.utrap && u.utraptype == TT_WEB) {
/*JP
        You("orient yourself on the web.");
*/
        You("��������Ŭ��������");
        u.utrap = 0;
    }
    check_strangling(TRUE); /* maybe start strangling */
    (void) polysense(youmonst.data);

    context.botl = 1;
    vision_full_recalc = 1;
    see_monsters();
    (void) encumber_msg();

    retouch_equipment(2);
    /* this might trigger a recursive call to polymon() [stone golem
       wielding cockatrice corpse and hit by stone-to-flesh, becomes
       flesh golem above, now gets transformed back into stone golem] */
    if (!uarmg)
        selftouch(no_longer_petrify_resistant);
    return 1;
}

STATIC_OVL void
break_armor()
{
    register struct obj *otmp;

    if (breakarm(youmonst.data)) {
        if ((otmp = uarm) != 0) {
            if (donning(otmp))
                cancel_don();
/*JP
            You("break out of your armor!");
*/
            You("�����������");
            exercise(A_STR, FALSE);
            (void) Armor_gone();
            useup(otmp);
        }
        if ((otmp = uarmc) != 0) {
            if (otmp->oartifact) {
/*JP
                Your("%s falls off!", cloak_simple_name(otmp));
*/
                Your("%s��æ���������", cloak_simple_name(otmp));
                (void) Cloak_off();
                dropx(otmp);
            } else {
/*JP
                Your("%s tears apart!", cloak_simple_name(otmp));
*/
                Your("%s�Ϥ��������˰��������줿��", cloak_simple_name(otmp));
                (void) Cloak_off();
                useup(otmp);
            }
        }
        if (uarmu) {
/*JP
            Your("shirt rips to shreds!");
*/
            Your("����Ĥϰ��������줿��");
            useup(uarmu);
        }
    } else if (sliparm(youmonst.data)) {
        if (((otmp = uarm) != 0) && (racial_exception(&youmonst, otmp) < 1)) {
            if (donning(otmp))
                cancel_don();
/*JP
            Your("armor falls around you!");
*/
            Your("���Ϥ��ʤ��Τޤ����������");
            (void) Armor_gone();
            dropx(otmp);
        }
        if ((otmp = uarmc) != 0) {
            if (is_whirly(youmonst.data))
/*JP
                Your("%s falls, unsupported!", cloak_simple_name(otmp));
*/
                Your("%s�Ϥ��Ȥ���������", cloak_simple_name(otmp));
            else
/*JP
                You("shrink out of your %s!", cloak_simple_name(otmp));
*/
                You("%s����̤߽Ф���", cloak_simple_name(otmp));
            (void) Cloak_off();
            dropx(otmp);
        }
        if ((otmp = uarmu) != 0) {
            if (is_whirly(youmonst.data))
/*JP
                You("seep right through your shirt!");
*/
                You("����Ĥ��餷�߽Ф���");
            else
/*JP
                You("become much too small for your shirt!");
*/
                You("����Ĥ�ꤺ�äȾ������ʤä���");
            setworn((struct obj *) 0, otmp->owornmask & W_ARMU);
            dropx(otmp);
        }
    }
    if (has_horns(youmonst.data)) {
        if ((otmp = uarmh) != 0) {
            if (is_flimsy(otmp) && !donning(otmp)) {
#if 0 /*JP*/
                char hornbuf[BUFSZ];

                /* Future possibilities: This could damage/destroy helmet */
                Sprintf(hornbuf, "horn%s", plur(num_horns(youmonst.data)));
                Your("%s %s through %s.", hornbuf, vtense(hornbuf, "pierce"),
                     yname(otmp));
#else
                Your("�Ѥ�%s��Ĥ�̤�����", yname(otmp));
#endif
            } else {
                if (donning(otmp))
                    cancel_don();
#if 0 /*JP*/
                Your("%s falls to the %s!", helm_simple_name(otmp),
                     surface(u.ux, u.uy));
#else
                Your("%s��%s���������", helm_simple_name(otmp),
                     surface(u.ux, u.uy));
#endif
                (void) Helmet_off();
                dropx(otmp);
            }
        }
    }
    if (nohands(youmonst.data) || verysmall(youmonst.data)) {
        if ((otmp = uarmg) != 0) {
            if (donning(otmp))
                cancel_don();
            /* Drop weapon along with gloves */
/*JP
            You("drop your gloves%s!", uwep ? " and weapon" : "");
*/
            You("����%s�������", uwep ? "�����" : "");
            drop_weapon(0);
            (void) Gloves_off();
            dropx(otmp);
        }
        if ((otmp = uarms) != 0) {
/*JP
            You("can no longer hold your shield!");
*/
            You("�⤦�����äƤ��ʤ���");
            (void) Shield_off();
            dropx(otmp);
        }
        if ((otmp = uarmh) != 0) {
            if (donning(otmp))
                cancel_don();
#if 0 /*JP*/
            Your("%s falls to the %s!", helm_simple_name(otmp),
                 surface(u.ux, u.uy));
#else
            Your("%s��%s���������", helm_simple_name(otmp),
                 surface(u.ux, u.uy));
#endif
            (void) Helmet_off();
            dropx(otmp);
        }
    }
    if (nohands(youmonst.data) || verysmall(youmonst.data)
        || slithy(youmonst.data) || youmonst.data->mlet == S_CENTAUR) {
        if ((otmp = uarmf) != 0) {
            if (donning(otmp))
                cancel_don();
            if (is_whirly(youmonst.data))
/*JP
                Your("boots fall away!");
*/
                Your("����æ���������");
            else
#if 0 /*JP*/
                Your("boots %s off your feet!",
                     verysmall(youmonst.data) ? "slide" : "are pushed");
#else
                Your("���Ϥ��ʤ���­����%s��",
                     verysmall(youmonst.data) ? "��������" : "æ�������");
#endif
            (void) Boots_off();
            dropx(otmp);
        }
    }
}

STATIC_OVL void
drop_weapon(alone)
int alone;
{
    struct obj *otmp;
    const char *what, *which, *whichtoo;
    boolean candropwep, candropswapwep;

    if (uwep) {
        /* !alone check below is currently superfluous but in the
         * future it might not be so if there are monsters which cannot
         * wear gloves but can wield weapons
         */
        if (!alone || cantwield(youmonst.data)) {
            candropwep = canletgo(uwep, "");
            candropswapwep = !u.twoweap || canletgo(uswapwep, "");
            if (alone) {
                what = (candropwep && candropswapwep) ? "drop" : "release";
                which = is_sword(uwep) ? "sword" : weapon_descr(uwep);
                if (u.twoweap) {
                    whichtoo =
                        is_sword(uswapwep) ? "sword" : weapon_descr(uswapwep);
                    if (strcmp(which, whichtoo))
                        which = "weapon";
                }
                if (uwep->quan != 1L || u.twoweap)
                    which = makeplural(which);

#if 0 /*JP*/
                You("find you must %s %s %s!", what,
                    the_your[!!strncmp(which, "corpse", 6)], which);
#else
                You("%s����Ȥ������Ȥ˵��Ť�����", what);
#endif
            }
            if (u.twoweap) {
                otmp = uswapwep;
                uswapwepgone();
                if (candropswapwep)
                    dropx(otmp);
            }
            otmp = uwep;
            uwepgone();
            if (candropwep)
                dropx(otmp);
            update_inventory();
        } else if (!could_twoweap(youmonst.data)) {
            untwoweapon();
        }
    }
}

void
rehumanize()
{
    /* You can't revert back while unchanging */
    if (Unchanging && (u.mh < 1)) {
        killer.format = NO_KILLER_PREFIX;
/*JP
        Strcpy(killer.name, "killed while stuck in creature form");
*/
        Strcpy(killer.name, "���λѤ���줺��");
        done(DIED);
    }

    if (emits_light(youmonst.data))
        del_light_source(LS_MONSTER, monst_to_any(&youmonst));
/*JP
    polyman("return to %s form!", urace.adj);
*/
    polyman("%s����ä���", urace.adj);

    if (u.uhp < 1) {
        /* can only happen if some bit of code reduces u.uhp
           instead of u.mh while poly'd */
/*JP
        Your("old form was not healthy enough to survive.");
*/
        Your("���λѤ������Ƥ����������Ϥ��ʤ���");
/*JP
        Sprintf(killer.name, "reverting to unhealthy %s form", urace.adj);
*/
        Sprintf(killer.name, "�Է򹯤�%s�λѤ���ä�", urace.adj);
        killer.format = KILLED_BY;
        done(DIED);
    }
    nomul(0);

    context.botl = 1;
    vision_full_recalc = 1;
    (void) encumber_msg();

    retouch_equipment(2);
    if (!uarmg)
        selftouch(no_longer_petrify_resistant);
}

int
dobreathe()
{
    struct attack *mattk;

    if (Strangled) {
/*JP
        You_cant("breathe.  Sorry.");
*/
        You_cant("©���Ǥ����Ȥ��Ǥ��ʤ�����ǰ��");
        return 0;
    }
    if (u.uen < 15) {
/*JP
        You("don't have enough energy to breathe!");
*/
        You("©���Ǥ��Τ˽�ʬ�ʥ��ͥ륮�����ʤ��ä���");
        return 0;
    }
    u.uen -= 15;
    context.botl = 1;

    if (!getdir((char *) 0))
        return 0;

    mattk = attacktype_fordmg(youmonst.data, AT_BREA, AD_ANY);
    if (!mattk)
        impossible("bad breath attack?"); /* mouthwash needed... */
    else if (!u.dx && !u.dy && !u.dz)
        ubreatheu(mattk);
    else
        buzz((int) (20 + mattk->adtyp - 1), (int) mattk->damn, u.ux, u.uy,
             u.dx, u.dy);
    return 1;
}

int
dospit()
{
    struct obj *otmp;
    struct attack *mattk;

    if (!getdir((char *) 0))
        return 0;
    mattk = attacktype_fordmg(youmonst.data, AT_SPIT, AD_ANY);
    if (!mattk) {
        impossible("bad spit attack?");
    } else {
        switch (mattk->adtyp) {
        case AD_BLND:
        case AD_DRST:
            otmp = mksobj(BLINDING_VENOM, TRUE, FALSE);
            break;
        default:
            impossible("bad attack type in dospit");
        /* fall through */
        case AD_ACID:
            otmp = mksobj(ACID_VENOM, TRUE, FALSE);
            break;
        }
        otmp->spe = 1; /* to indicate it's yours */
        throwit(otmp, 0L, FALSE);
    }
    return 1;
}

int
doremove()
{
    if (!Punished) {
        if (u.utrap && u.utraptype == TT_BURIEDBALL) {
#if 0 /*JP*/
            pline_The("ball and chain are buried firmly in the %s.",
                      surface(u.ux, u.uy));
#else
            pline_The("��Ⱥ���%s�ˤ��ä������ޤäƤ��롥.",
                      surface(u.ux, u.uy));
#endif
            return 0;
        }
/*JP
        You("are not chained to anything!");
*/
        You("����Ĥʤ���Ƥ��ʤ���");
        return 0;
    }
    unpunish();
    return 1;
}

int
dospinweb()
{
    register struct trap *ttmp = t_at(u.ux, u.uy);

    if (Levitation || Is_airlevel(&u.uz) || Underwater
        || Is_waterlevel(&u.uz)) {
/*JP
        You("must be on the ground to spin a web.");
*/
        You("��������ĥ��ˤ����̤ξ�ˤ��ʤ��ƤϤʤ�ʤ���");
        return 0;
    }
    if (u.uswallow) {
/*JP
        You("release web fluid inside %s.", mon_nam(u.ustuck));
*/
        You("%s����Ǥ��������Ǥ��Ф�����", mon_nam(u.ustuck));
        if (is_animal(u.ustuck->data)) {
            expels(u.ustuck, u.ustuck->data, TRUE);
            return 0;
        }
        if (is_whirly(u.ustuck->data)) {
            int i;

            for (i = 0; i < NATTK; i++)
                if (u.ustuck->data->mattk[i].aatyp == AT_ENGL)
                    break;
            if (i == NATTK)
                impossible("Swallower has no engulfing attack?");
            else {
                char sweep[30];

                sweep[0] = '\0';
                switch (u.ustuck->data->mattk[i].adtyp) {
                case AD_FIRE:
/*JP
                    Strcpy(sweep, "ignites and ");
*/
                    Strcpy(sweep, "ȯ�Ф�");
                    break;
                case AD_ELEC:
/*JP
                    Strcpy(sweep, "fries and ");
*/
                    Strcpy(sweep, "�Ǥ�");
                    break;
                case AD_COLD:
/*JP
                    Strcpy(sweep, "freezes, shatters and ");
*/
                    Strcpy(sweep, "���Ĥ������ʤ��ʤˤʤ�");
                    break;
                }
/*JP
                pline_The("web %sis swept away!", sweep);
*/
                pline("��������%s�ʤ��ʤä���", sweep);
            }
            return 0;
        } /* default: a nasty jelly-like creature */
/*JP
        pline_The("web dissolves into %s.", mon_nam(u.ustuck));
*/
        pline("��������ʬ�򤷤�%s�ˤʤä���", mon_nam(u.ustuck));
        return 0;
    }
    if (u.utrap) {
/*JP
        You("cannot spin webs while stuck in a trap.");
*/
        You("櫤ˤϤޤäƤ���֤Ϥ�������ĥ��ʤ���");
        return 0;
    }
    exercise(A_DEX, TRUE);
    if (ttmp) {
        switch (ttmp->ttyp) {
        case PIT:
        case SPIKED_PIT:
/*JP
            You("spin a web, covering up the pit.");
*/
            You("��������ĥ�ꡤ����ʤ�ä���");
            deltrap(ttmp);
            bury_objs(u.ux, u.uy);
            newsym(u.ux, u.uy);
            return 1;
        case SQKY_BOARD:
/*JP
            pline_The("squeaky board is muffled.");
*/
            pline("�������Ĥ�ʤ��줿��");
            deltrap(ttmp);
            newsym(u.ux, u.uy);
            return 1;
        case TELEP_TRAP:
        case LEVEL_TELEP:
        case MAGIC_PORTAL:
        case VIBRATING_SQUARE:
/*JP
            Your("webbing vanishes!");
*/
            Your("�������Ͼä�����");
            return 0;
        case WEB:
/*JP
            You("make the web thicker.");
*/
            You("�������������������");
            return 1;
        case HOLE:
        case TRAPDOOR:
#if 0 /*JP*/
            You("web over the %s.",
                (ttmp->ttyp == TRAPDOOR) ? "trap door" : "hole");
#else
            You("%s�򤯤�����ʤ�ä���",
                (ttmp->ttyp == TRAPDOOR) ? "���" : "��");
#endif
            deltrap(ttmp);
            newsym(u.ux, u.uy);
            return 1;
        case ROLLING_BOULDER_TRAP:
/*JP
            You("spin a web, jamming the trigger.");
*/
            You("��������ĥ�äơ������å���ư���ʤ�������");
            deltrap(ttmp);
            newsym(u.ux, u.uy);
            return 1;
        case ARROW_TRAP:
        case DART_TRAP:
        case BEAR_TRAP:
        case ROCKTRAP:
        case FIRE_TRAP:
        case LANDMINE:
        case SLP_GAS_TRAP:
        case RUST_TRAP:
        case MAGIC_TRAP:
        case ANTI_MAGIC:
        case POLY_TRAP:
/*JP
            You("have triggered a trap!");
*/
            You("櫤��ư�����Ƥ��ޤä���");
            dotrap(ttmp, 0);
            return 1;
        default:
            impossible("Webbing over trap type %d?", ttmp->ttyp);
            return 0;
        }
    } else if (On_stairs(u.ux, u.uy)) {
        /* cop out: don't let them hide the stairs */
#if 0 /*JP:T*/
        Your("web fails to impede access to the %s.",
             (levl[u.ux][u.uy].typ == STAIRS) ? "stairs" : "ladder");
#else
        Your("��������%s�ؤΰ�ư�����Ǥ��ʤ���",
             (levl[u.ux][u.uy].typ == STAIRS) ? "����" : "�Ϥ���");
#endif
        return 1;
    }
    ttmp = maketrap(u.ux, u.uy, WEB);
    if (ttmp) {
        ttmp->madeby_u = 1;
        feeltrap(ttmp);
    }
    return 1;
}

int
dosummon()
{
    int placeholder;
    if (u.uen < 10) {
/*JP
        You("lack the energy to send forth a call for help!");
*/
        You("������Ƥ֤��������Ϥ��ʤ���");
        return 0;
    }
    u.uen -= 10;
    context.botl = 1;

/*JP
    You("call upon your brethren for help!");
*/
    You("��֤�Ƥ����");
    exercise(A_WIS, TRUE);
    if (!were_summon(youmonst.data, TRUE, &placeholder, (char *) 0))
/*JP
        pline("But none arrive.");
*/
        pline("��������������ʤ���");
    return 1;
}

int
dogaze()
{
    register struct monst *mtmp;
    int looked = 0;
    char qbuf[QBUFSZ];
    int i;
    uchar adtyp = 0;

    for (i = 0; i < NATTK; i++) {
        if (youmonst.data->mattk[i].aatyp == AT_GAZE) {
            adtyp = youmonst.data->mattk[i].adtyp;
            break;
        }
    }
    if (adtyp != AD_CONF && adtyp != AD_FIRE) {
        impossible("gaze attack %d?", adtyp);
        return 0;
    }

    if (Blind) {
/*JP
        You_cant("see anything to gaze at.");
*/
        You("�ܤ������ʤ��Τǡ��ˤ��ʤ���");
        return 0;
    } else if (Hallucination) {
/*JP
        You_cant("gaze at anything you can see.");
*/
        You_cant("�������Τ򲿤�ˤ��ʤ���");
        return 0;
    }
    if (u.uen < 15) {
/*JP
        You("lack the energy to use your special gaze!");
*/
        You("�ˤ����������Ϥ��ʤ���");
        return 0;
    }
    u.uen -= 15;
    context.botl = 1;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (canseemon(mtmp) && couldsee(mtmp->mx, mtmp->my)) {
            looked++;
            if (Invis && !perceives(mtmp->data)) {
/*JP
                pline("%s seems not to notice your gaze.", Monnam(mtmp));
*/
                pline("%s�Ϥ��ʤ��Τˤ�ߤ˵����Ĥ��Ƥʤ��褦����", Monnam(mtmp));
            } else if (mtmp->minvis && !See_invisible) {
/*JP
                You_cant("see where to gaze at %s.", Monnam(mtmp));
*/
                You("%s�ϸ����ʤ��Τǡ��ˤ��ʤ�", Monnam(mtmp));
            } else if (mtmp->m_ap_type == M_AP_FURNITURE
                       || mtmp->m_ap_type == M_AP_OBJECT) {
                looked--;
                continue;
            } else if (flags.safe_dog && mtmp->mtame && !Confusion) {
/*JP
                You("avoid gazing at %s.", y_monnam(mtmp));
*/
                You("%s�����ܤ򤽤餷�Ƥ��ޤä���", y_monnam(mtmp));
            } else {
                if (flags.confirm && mtmp->mpeaceful && !Confusion) {
#if 0 /*JP*/
                    Sprintf(qbuf, "Really %s %s?",
                            (adtyp == AD_CONF) ? "confuse" : "attack",
                            mon_nam(mtmp));
#else
                    Sprintf(qbuf, "������%s��%s��",
                            mon_nam(mtmp),
                            (adtyp == AD_CONF) ? "���𤵤���" : "���⤹��");
#endif
                    if (yn(qbuf) != 'y')
                        continue;
                    setmangry(mtmp);
                }
                if (!mtmp->mcanmove || mtmp->mstun || mtmp->msleeping
                    || !mtmp->mcansee || !haseyes(mtmp->data)) {
                    looked--;
                    continue;
                }
                /* No reflection check for consistency with when a monster
                 * gazes at *you*--only medusa gaze gets reflected then.
                 */
                if (adtyp == AD_CONF) {
                    if (!mtmp->mconf)
/*JP
                        Your("gaze confuses %s!", mon_nam(mtmp));
*/
                        Your("�ˤ�ߤ�%s���𤵤�����", mon_nam(mtmp));
                    else
/*JP
                        pline("%s is getting more and more confused.",
*/
                        pline("%s�Ϥޤ��ޤ����𤷤���",
                              Monnam(mtmp));
                    mtmp->mconf = 1;
                } else if (adtyp == AD_FIRE) {
                    int dmg = d(2, 6), lev = (int) u.ulevel;

/*JP
                    You("attack %s with a fiery gaze!", mon_nam(mtmp));
*/
                    You("��Τˤ�ߤ�%s�򹶷⤷����", mon_nam(mtmp));
                    if (resists_fire(mtmp)) {
/*JP
                        pline_The("fire doesn't burn %s!", mon_nam(mtmp));
*/
                        pline("%s�ϱ��ǳ���ʤ��ä���", mon_nam(mtmp));
                        dmg = 0;
                    }
                    if (lev > rn2(20))
                        (void) destroy_mitem(mtmp, SCROLL_CLASS, AD_FIRE);
                    if (lev > rn2(20))
                        (void) destroy_mitem(mtmp, POTION_CLASS, AD_FIRE);
                    if (lev > rn2(25))
                        (void) destroy_mitem(mtmp, SPBOOK_CLASS, AD_FIRE);
                    if (dmg)
                        mtmp->mhp -= dmg;
                    if (mtmp->mhp <= 0)
                        killed(mtmp);
                }
                /* For consistency with passive() in uhitm.c, this only
                 * affects you if the monster is still alive.
                 */
                if (DEADMONSTER(mtmp))
                    continue;

                if (mtmp->data == &mons[PM_FLOATING_EYE] && !mtmp->mcan) {
                    if (!Free_action) {
/*JP
                        You("are frozen by %s gaze!",
*/
                        You("%s�Τˤ�ߤ�ư���ʤ��ʤä���", 
                            s_suffix(mon_nam(mtmp)));
                        nomul((u.ulevel > 6 || rn2(4))
                                  ? -d((int) mtmp->m_lev + 1,
                                       (int) mtmp->data->mattk[0].damd)
                                  : -200);
                        multi_reason = "frozen by a monster's gaze";
                        nomovemsg = 0;
                        return 1;
                    } else
/*JP
                        You("stiffen momentarily under %s gaze.",
*/
                        You("%s�Τˤ�ߤǰ�ֹ�ľ������",
                            s_suffix(mon_nam(mtmp)));
                }
                /* Technically this one shouldn't affect you at all because
                 * the Medusa gaze is an active monster attack that only
                 * works on the monster's turn, but for it to *not* have an
                 * effect would be too weird.
                 */
                if (mtmp->data == &mons[PM_MEDUSA] && !mtmp->mcan) {
/*JP
                    pline("Gazing at the awake %s is not a very good idea.",
*/
                    pline("�ܤ�Фޤ��Ƥ���%s��ˤ��Τϸ������Ȥ���ʤ���",
                          l_monnam(mtmp));
                    /* as if gazing at a sleeping anything is fruitful... */
/*JP
                    You("turn to stone...");
*/
                    You("�Фˤʤä�������");
                    killer.format = KILLED_BY;
/*JP
                    Strcpy(killer.name, "deliberately meeting Medusa's gaze");
*/
                    Strcpy(killer.name, "�虜�虜��ǥ塼���Τˤ�ߤ�ޤȤ�˸���");
                    done(STONING);
                }
            }
        }
    }
    if (!looked)
/*JP
        You("gaze at no place in particular.");
*/
        You("�ºݤˤϲ���ˤ��ʤ��ä���");
    return 1;
}

int
dohide()
{
    boolean ismimic = youmonst.data->mlet == S_MIMIC,
            on_ceiling = is_clinger(youmonst.data) || Flying;

    /* can't hide while being held (or holding) or while trapped
       (except for floor hiders [trapper or mimic] in pits) */
    if (u.ustuck || (u.utrap && (u.utraptype != TT_PIT || on_ceiling))) {
        You_cant("hide while you're %s.",
                 !u.ustuck ? "trapped" : !sticks(youmonst.data)
                                             ? "being held"
                                             : humanoid(u.ustuck->data)
                                                   ? "holding someone"
                                                   : "holding that creature");
        if (u.uundetected
            || (ismimic && youmonst.m_ap_type != M_AP_NOTHING)) {
            u.uundetected = 0;
            youmonst.m_ap_type = M_AP_NOTHING;
            newsym(u.ux, u.uy);
        }
        return 0;
    }
    /* note: the eel and hides_under cases are hypothetical;
       such critters aren't offered the option of hiding via #monster */
    if (youmonst.data->mlet == S_EEL && !is_pool(u.ux, u.uy)) {
        if (IS_FOUNTAIN(levl[u.ux][u.uy].typ))
            The("fountain is not deep enough to hide in.");
        else
            There("is no water to hide in here.");
        u.uundetected = 0;
        return 0;
    }
    if (hides_under(youmonst.data) && !level.objects[u.ux][u.uy]) {
        There("is nothing to hide under here.");
        u.uundetected = 0;
        return 0;
    }
    /* Planes of Air and Water */
    if (on_ceiling && !has_ceiling(&u.uz)) {
        There("is nowhere to hide above you.");
        u.uundetected = 0;
        return 0;
    }
    if ((is_hider(youmonst.data) && !Flying) /* floor hider */
        && (Is_airlevel(&u.uz) || Is_waterlevel(&u.uz))) {
        There("is nowhere to hide beneath you.");
        u.uundetected = 0;
        return 0;
    }
    /* TODO? inhibit floor hiding at furniture locations, or
     * else make youhiding() give smarter messages at such spots.
     */

    if (u.uundetected || (ismimic && youmonst.m_ap_type != M_AP_NOTHING)) {
        youhiding(FALSE, 1); /* "you are already hiding" */
        return 0;
    }

    if (ismimic) {
        /* should bring up a dialog "what would you like to imitate?" */
        youmonst.m_ap_type = M_AP_OBJECT;
        youmonst.mappearance = STRANGE_OBJECT;
    } else
        u.uundetected = 1;
    newsym(u.ux, u.uy);
    youhiding(FALSE, 0); /* "you are now hiding" */
    return 1;
}

int
dopoly()
{
    struct permonst *savedat = youmonst.data;

    if (is_vampire(youmonst.data)) {
        polyself(2);
        if (savedat != youmonst.data) {
            You("transform into %s.", an(youmonst.data->mname));
            newsym(u.ux, u.uy);
        }
    }
    return 1;
}

int
domindblast()
{
    struct monst *mtmp, *nmon;

    if (u.uen < 10) {
/*JP
        You("concentrate but lack the energy to maintain doing so.");
*/
        You("���椷�������������ͥ륮����­��ʤ���");
        return 0;
    }
    u.uen -= 10;
    context.botl = 1;

/*JP
    You("concentrate.");
*/
    You("���椷����");
/*JP
    pline("A wave of psychic energy pours out.");
*/
    pline("�������ͥ륮���Ȥ�����������");
    for (mtmp = fmon; mtmp; mtmp = nmon) {
        int u_sen;

        nmon = mtmp->nmon;
        if (DEADMONSTER(mtmp))
            continue;
        if (distu(mtmp->mx, mtmp->my) > BOLT_LIM * BOLT_LIM)
            continue;
        if (mtmp->mpeaceful)
            continue;
        u_sen = telepathic(mtmp->data) && !mtmp->mcansee;
        if (u_sen || (telepathic(mtmp->data) && rn2(2)) || !rn2(10)) {
#if 0 /*JP*/
            You("lock in on %s %s.", s_suffix(mon_nam(mtmp)),
                u_sen ? "telepathy"
                      : telepathic(mtmp->data) ? "latent telepathy" : "mind");
#else
            pline("%s��%s��������", mon_nam(mtmp),
                u_sen ? "��������"
                      : telepathic(mtmp->data) ? "����Ū��������" : "���ذռ�����");
#endif
            mtmp->mhp -= rnd(15);
            if (mtmp->mhp <= 0)
                killed(mtmp);
        }
    }
    return 1;
}

STATIC_OVL void
uunstick()
{
/*JP
    pline("%s is no longer in your clutches.", Monnam(u.ustuck));
*/
    pline("%s�Ϥ��ʤ��μ꤫��ƨ�줿��", Monnam(u.ustuck));
    u.ustuck = 0;
}

void
skinback(silently)
boolean silently;
{
    if (uskin) {
        if (!silently)
/*JP
            Your("skin returns to its original form.");
*/
            Your("���������λѤ���ä���");
        uarm = uskin;
        uskin = (struct obj *) 0;
        /* undo save/restore hack */
        uarm->owornmask &= ~I_SPECIAL;
    }
}

const char *
mbodypart(mon, part)
struct monst *mon;
int part;
{
    static NEARDATA const char
#if 0 /*JP*/
        *humanoid_parts[] = { "arm",       "eye",  "face",         "finger",
                              "fingertip", "foot", "hand",         "handed",
                              "head",      "leg",  "light headed", "neck",
                              "spine",     "toe",  "hair",         "blood",
                              "lung",      "nose", "stomach" },
#else
        *humanoid_parts[] = { "��", "��", "��", "��",
            "����", "­", "��", "��ˤ���",
            "Ƭ", "­", "��ޤ�������", "��",
            "�ع�", "����", "ȱ",  "��",
            "��", "ɡ", "��"},
#endif
#if 0 /*JP*/
        *jelly_parts[] = { "pseudopod", "dark spot", "front",
                           "pseudopod extension", "pseudopod extremity",
                           "pseudopod root", "grasp", "grasped",
                           "cerebral area", "lower pseudopod", "viscous",
                           "middle", "surface", "pseudopod extremity",
                           "ripples", "juices", "surface", "sensor",
                           "stomach" },
#else
    *jelly_parts[] = { "��������", "��������", "����",
        "�����������", "��������",
        "��������δ�", "����", "����",
        "Ǿ���ΰ�", "�����ε�������", "�ͤФͤФ��Ƥ���",
        "����ΰ�", "ɽ��",  "��������",
        "����", "�α�", "ɽ��", "���д�",
        "��"},
#endif
#if 0 /*JP*/
        *animal_parts[] = { "forelimb",  "eye",           "face",
                            "foreclaw",  "claw tip",      "rear claw",
                            "foreclaw",  "clawed",        "head",
                            "rear limb", "light headed",  "neck",
                            "spine",     "rear claw tip", "fur",
                            "blood",     "lung",          "nose",
                            "stomach" },
#else
    *animal_parts[] = { "��­", "��", "��",
        "����", "����", "����",
        "����", "�Ҥä�����", "Ƭ",
        "��­", "��ޤ�������", "��",
        "�ع�", "������", "����",
        "��", "��", "ɡ",
        "��"},
#endif
#if 0 /*JP*/
        *bird_parts[] = { "wing",     "eye",  "face",         "wing",
                          "wing tip", "foot", "wing",         "winged",
                          "head",     "leg",  "light headed", "neck",
                          "spine",    "toe",  "feathers",     "blood",
                          "lung",     "bill", "stomach" },
#else
    *bird_parts[] = { "��", "��", "��", "��",
        "�����", "­", "��", "��ˤȤ�",
        "Ƭ", "­", "��ޤ�������", "��",
        "�ع�", "����", "����", "��",
        "��", "�����Ф�", "��" },
#endif
#if 0 /*JP*/
        *horse_parts[] = { "foreleg",  "eye",           "face",
                           "forehoof", "hoof tip",      "rear hoof",
                           "forehoof", "hooved",        "head",
                           "rear leg", "light headed",  "neck",
                           "backbone", "rear hoof tip", "mane",
                           "blood",    "lung",          "nose",
                           "stomach" },
#else
    *horse_parts[] = { "��­", "��", "��",
        "����", "��", "����",
        "����", "���ˤϤ���", "Ƭ",
        "��­", "��ޤ�������", "��",
        "�ع�", "������", "���Ƥ���",
        "��", "��", "ɡ",
        "��" },
#endif
#if 0 /*JP*/
        *sphere_parts[] = { "appendage", "optic nerve", "body", "tentacle",
                            "tentacle tip", "lower appendage", "tentacle",
                            "tentacled", "body", "lower tentacle",
                            "rotational", "equator", "body",
                            "lower tentacle tip", "cilia", "life force",
                            "retina", "olfactory nerve", "interior" },
#else
        *sphere_parts[] = { "�͵�", "��п���", "��", "����",
            "�������", "�����͵�", "����",
            "����˻���", "��", "���ο���",
            "��ž����", "�濴��", "��",
            "���ο������", "����", "��̿��",
            "����", "�̳����", "����" },
#endif
#if 0 /*JP*/
        *fungus_parts[] = { "mycelium", "visual area", "front",
                            "hypha",    "hypha",       "root",
                            "strand",   "stranded",    "cap area",
                            "rhizome",  "sporulated",  "stalk",
                            "root",     "rhizome tip", "spores",
                            "juices",   "gill",        "gill",
                            "interior" },
#else
        *fungus_parts[] = { "�ݻ���", "����ΰ�", "��",
            "�ݻ�", "�ݻ�", "��",
            "����", "����ˤ���ߤĤ���", "��",
            "����", "���𤹤�", "��",
            "��", "���Ԥ���", "��˦",
            "�α�", "����", "����",
            "����"},
#endif
#if 0 /*JP*/
        *vortex_parts[] = { "region",        "eye",           "front",
                            "minor current", "minor current", "lower current",
                            "swirl",         "swirled",       "central core",
                            "lower current", "addled",        "center",
                            "currents",      "edge",          "currents",
                            "life force",    "center",        "leading edge",
                            "interior" },
#else
        *vortex_parts[] = { "�ΰ�", "��", "��",
            "������ή��", "������ή��", "������ή��",
            "����", "���˴���", "�����濴",
            "������ή��", "���𤷤�", "�濴��",
            "ή��", "����", "��ή",
            "��̿��", "�濴", "����",
            "����" },
#endif
#if 0 /*JP*/
        *snake_parts[] = { "vestigial limb", "eye", "face", "large scale",
                           "large scale tip", "rear region", "scale gap",
                           "scale gapped", "head", "rear region",
                           "light headed", "neck", "length", "rear scale",
                           "scales", "blood", "lung", "forked tongue",
                           "stomach" },
#else
        *snake_parts[] = { "�ಽ����­", "��", "��", "�礭����",
            "�礭���ڤ���", "����ʬ", "�ڤη��",
            "�ڤη�֤ˤĤ���", "Ƭ", "����ʬ",
            "��ޤ�������", "��", "��", "����ʬ�γ�",
            "��", "��", "��", "��",
            "��" },
#endif
#if 0 /*JP*/
        *worm_parts[] = { "anterior segment", "light sensitive cell",
                          "clitellum", "setae", "setae", "posterior segment",
                          "segment", "segmented", "anterior segment",
                          "posterior", "over stretched", "clitellum",
                          "length", "posterior setae", "setae", "blood",
                          "skin", "prostomium", "stomach" },
#else
        *worm_parts[] = { "����", "��������˦",
                          "����", "��", "��", "���",
                          "��", "��ˤĤ���", "����",
                          "����", "���Ӥ�����", "����",
                          "��", "�����γ�", "��", "��",
                          "����", "������", "��" },
#endif
#if 0 /*JP*/
        *fish_parts[] = { "fin", "eye", "premaxillary", "pelvic axillary",
                          "pelvic fin", "anal fin", "pectoral fin", "finned",
                          "head", "peduncle", "played out", "gills",
                          "dorsal fin", "caudal fin", "scales", "blood",
                          "gill", "nostril", "stomach" };
#else
        *fish_parts[] = { "�Ҥ�", "��", "��", "�Ҥ����",
            "�Ҥ����", "���Ӥ�", "���Ҥ�", "�Ҥ�ǻ���",
            "Ƭ", "����", "��ޤ�������", "����",
            "�ؤӤ�", "���Ӥ�", "��", "��",
            "����", "ɡ", "��" };
#endif
#if 0 /*JP*//*�Ȥ�ʤ�*/
    /* claw attacks are overloaded in mons[]; most humanoids with
       such attacks should still reference hands rather than claws */
    static const char not_claws[] = {
        S_HUMAN,     S_MUMMY,   S_ZOMBIE, S_ANGEL, S_NYMPH, S_LEPRECHAUN,
        S_QUANTMECH, S_VAMPIRE, S_ORC,    S_GIANT, /* quest nemeses */
        '\0' /* string terminator; assert( S_xxx != 0 ); */
    };
#endif
    struct permonst *mptr = mon->data;

#if 0 /*JP*/
/* paw�ϸ��Ȥ�ǭ�μꡤclaw�ϥ�����­�Τ褦�ʤ����Ĥᡤ
   �ɤ�������ܸ줸��ּ�פǤ����Ǥ��礦��
*/
    /* some special cases */
    if (mptr->mlet == S_DOG || mptr->mlet == S_FELINE
        || mptr->mlet == S_RODENT || mptr == &mons[PM_OWLBEAR]) {
        switch (part) {
        case HAND:
            return "paw";
        case HANDED:
            return "pawed";
        case FOOT:
            return "rear paw";
        case ARM:
        case LEG:
            return horse_parts[part]; /* "foreleg", "rear leg" */
        default:
            break; /* for other parts, use animal_parts[] below */
        }
    } else if (mptr->mlet == S_YETI) { /* excl. owlbear due to 'if' above */
        /* opposable thumbs, hence "hands", "arms", "legs", &c */
        return humanoid_parts[part]; /* yeti/sasquatch, monkey/ape */
    }
    if ((part == HAND || part == HANDED)
        && (humanoid(mptr) && attacktype(mptr, AT_CLAW)
            && !index(not_claws, mptr->mlet) && mptr != &mons[PM_STONE_GOLEM]
            && mptr != &mons[PM_INCUBUS] && mptr != &mons[PM_SUCCUBUS]))
        return (part == HAND) ? "claw" : "clawed";
#endif
#if 0 /*JP*//*trunk�Ͼݤ�ɡ���̣���뤽���Ǥ������ܸ�Ǥ�ñ��ɡ�Ǥ������ȡ�*/
    if ((mptr == &mons[PM_MUMAK] || mptr == &mons[PM_MASTODON])
        && part == NOSE)
        return "trunk";
#endif
    if (mptr == &mons[PM_SHARK] && part == HAIR)
#if 0 /*JP*/
        return "skin"; /* sharks don't have scales */
#else
        return "Ƭ"; /* sharks don't have scales */
#endif
    if ((mptr == &mons[PM_JELLYFISH] || mptr == &mons[PM_KRAKEN])
        && (part == ARM || part == FINGER || part == HAND || part == FOOT
            || part == TOE))
/*JP
        return "tentacle";
*/
        return "����";
    if (mptr == &mons[PM_FLOATING_EYE] && part == EYE)
/*JP
        return "cornea";
*/
        return "����";
    if (humanoid(mptr) && (part == ARM || part == FINGER || part == FINGERTIP
                           || part == HAND || part == HANDED))
        return humanoid_parts[part];
    if (mptr == &mons[PM_RAVEN])
        return bird_parts[part];
    if (mptr->mlet == S_CENTAUR || mptr->mlet == S_UNICORN
        || (mptr == &mons[PM_ROTHE] && part != HAIR))
        return horse_parts[part];
    if (mptr->mlet == S_LIGHT) {
#if 0 /*JP*/
        if (part == HANDED)
            return "rayed";
        else if (part == ARM || part == FINGER || part == FINGERTIP
                 || part == HAND)
            return "ray";
        else
            return "beam";
#else
        if (part == HANDED || part == ARM || part == FINGER
            || part == FINGERTIP || part == HAND) {
            return "��";
        }
#endif
    }
    if (mptr == &mons[PM_STALKER] && part == HEAD)
        return "head";
    if (mptr->mlet == S_EEL && mptr != &mons[PM_JELLYFISH])
        return fish_parts[part];
    if (mptr->mlet == S_WORM)
        return worm_parts[part];
    if (slithy(mptr) || (mptr->mlet == S_DRAGON && part == HAIR))
        return snake_parts[part];
    if (mptr->mlet == S_EYE)
        return sphere_parts[part];
    if (mptr->mlet == S_JELLY || mptr->mlet == S_PUDDING
        || mptr->mlet == S_BLOB || mptr == &mons[PM_JELLYFISH])
        return jelly_parts[part];
    if (mptr->mlet == S_VORTEX || mptr->mlet == S_ELEMENTAL)
        return vortex_parts[part];
    if (mptr->mlet == S_FUNGUS)
        return fungus_parts[part];
    if (humanoid(mptr))
        return humanoid_parts[part];
    return animal_parts[part];
}

const char *
body_part(part)
int part;
{
    return mbodypart(&youmonst, part);
}

int
poly_gender()
{
    /* Returns gender of polymorphed player;
     * 0/1=same meaning as flags.female, 2=none.
     */
    if (is_neuter(youmonst.data) || !humanoid(youmonst.data))
        return 2;
    return flags.female;
}

void
ugolemeffects(damtype, dam)
int damtype, dam;
{
    int heal = 0;

    /* We won't bother with "slow"/"haste" since players do not
     * have a monster-specific slow/haste so there is no way to
     * restore the old velocity once they are back to human.
     */
    if (u.umonnum != PM_FLESH_GOLEM && u.umonnum != PM_IRON_GOLEM)
        return;
    switch (damtype) {
    case AD_ELEC:
        if (u.umonnum == PM_FLESH_GOLEM)
            heal = (dam + 5) / 6; /* Approx 1 per die */
        break;
    case AD_FIRE:
        if (u.umonnum == PM_IRON_GOLEM)
            heal = dam;
        break;
    }
    if (heal && (u.mh < u.mhmax)) {
        u.mh += heal;
        if (u.mh > u.mhmax)
            u.mh = u.mhmax;
        context.botl = 1;
/*JP
        pline("Strangely, you feel better than before.");
*/
        pline("��̯�ʤ��Ȥˡ�����굤ʬ���褯�ʤä���");
        exercise(A_STR, TRUE);
    }
}

STATIC_OVL int
armor_to_dragon(atyp)
int atyp;
{
    switch (atyp) {
    case GRAY_DRAGON_SCALE_MAIL:
    case GRAY_DRAGON_SCALES:
        return PM_GRAY_DRAGON;
    case SILVER_DRAGON_SCALE_MAIL:
    case SILVER_DRAGON_SCALES:
        return PM_SILVER_DRAGON;
#if 0 /* DEFERRED */
    case SHIMMERING_DRAGON_SCALE_MAIL:
    case SHIMMERING_DRAGON_SCALES:
        return PM_SHIMMERING_DRAGON;
#endif
    case RED_DRAGON_SCALE_MAIL:
    case RED_DRAGON_SCALES:
        return PM_RED_DRAGON;
    case ORANGE_DRAGON_SCALE_MAIL:
    case ORANGE_DRAGON_SCALES:
        return PM_ORANGE_DRAGON;
    case WHITE_DRAGON_SCALE_MAIL:
    case WHITE_DRAGON_SCALES:
        return PM_WHITE_DRAGON;
    case BLACK_DRAGON_SCALE_MAIL:
    case BLACK_DRAGON_SCALES:
        return PM_BLACK_DRAGON;
    case BLUE_DRAGON_SCALE_MAIL:
    case BLUE_DRAGON_SCALES:
        return PM_BLUE_DRAGON;
    case GREEN_DRAGON_SCALE_MAIL:
    case GREEN_DRAGON_SCALES:
        return PM_GREEN_DRAGON;
    case YELLOW_DRAGON_SCALE_MAIL:
    case YELLOW_DRAGON_SCALES:
        return PM_YELLOW_DRAGON;
    default:
        return -1;
    }
}

/*
 * Some species have awareness of other species
 */
static boolean
polysense(mptr)
struct permonst *mptr;
{
    short warnidx = 0;

    context.warntype.speciesidx = 0;
    context.warntype.species = 0;
    context.warntype.polyd = 0;

    switch (monsndx(mptr)) {
    case PM_PURPLE_WORM:
        warnidx = PM_SHRIEKER;
        break;
    case PM_VAMPIRE:
    case PM_VAMPIRE_LORD:
        context.warntype.polyd = M2_HUMAN | M2_ELF;
        HWarn_of_mon |= FROMRACE;
        return TRUE;
    }
    if (warnidx) {
        context.warntype.speciesidx = warnidx;
        context.warntype.species = &mons[warnidx];
        HWarn_of_mon |= FROMRACE;
        return TRUE;
    }
    context.warntype.speciesidx = 0;
    context.warntype.species = 0;
    HWarn_of_mon &= ~FROMRACE;
    return FALSE;
}

/*polyself.c*/
