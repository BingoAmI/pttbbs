/*
 * piaip's simplified implementation of TELNET protocol
 * Copyright (c) 2005-2009 Hung-Te Lin <piaip@csie.ntu.edu.tw>
 * Improved by Kuang 2009
 */

// #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#ifdef DEBUG
#define TELOPTS
#define TELCMDS
#endif
#include <arpa/telnet.h>

#include "cmsys.h"

static unsigned int telnet_handler(TelnetCtx *ctx, unsigned char c) ;
static void         telnet_write  (TelnetCtx *ctx, const void *buf, size_t nbytes);

enum TELNET_IAC_STATES {
	IAC_NONE,
	IAC_COMMAND,
	IAC_WAIT_OPT,
	IAC_WAIT_SE,
	IAC_PROCESS_OPT,
	IAC_ERROR
};

/* We don't reply to most commands, so this maxlen can be minimal.
 * Warning: if you want to support ENV passing or other long commands,
 * remember to increase this value. Howver, some poorly implemented
 * terminals like xxMan may not follow the protocols and user will hang
 * on those terminals when IACs were sent.
 */

TelnetCtx *telnet_create_contex(void)
{
    return (TelnetCtx*) malloc(sizeof(TelnetCtx));
}

void telnet_free_context(TelnetCtx* ctx)
{
    free(ctx);
}

void telnet_ctx_init(TelnetCtx *ctx, const struct TelnetCallback *callback, int fd)
{
    memset(ctx, 0, sizeof(TelnetCtx));

    // callback structure must be provided.
    assert(callback);
    ctx->callback = callback;
    ctx->fd = fd;
}

void telnet_ctx_set_cc_arg(TelnetCtx *ctx, void *arg)
{
    ctx->cc_arg = arg;
}

void telnet_ctx_set_write_arg(TelnetCtx *ctx, void *arg)
{
    ctx->write_arg = arg;
}

void telnet_ctx_set_resize_arg(TelnetCtx *ctx, void *arg)
{
    ctx->resize_arg = arg;
}

void telnet_ctx_set_ayt_arg(TelnetCtx *ctx, void *arg)
{
    ctx->ayt_arg = arg;
}

void telnet_ctx_set_ttype_arg (TelnetCtx *ctx, void *arg)
{
    ctx->ttype_arg = arg;
}


/* We are the boss. We don't respect to client.
 * It's client's responsibility to follow us.
 * Please write these codes in i-dont-care opt handlers.
 */
static const char telnet_init_cmds[] = {
    /* retrieve terminal type and throw away.
     * why? because without this, clients enter line mode.
     */
    IAC, DO, TELOPT_TTYPE,
    IAC, SB, TELOPT_TTYPE, TELQUAL_SEND, IAC, SE,

    /* i'm a smart term with resize ability. */
    IAC, DO, TELOPT_NAWS,

    /* i will echo. */
    IAC, WILL, TELOPT_ECHO,
    /* supress ga. */
    IAC, WILL, TELOPT_SGA,
    /* 8 bit binary. */
    IAC, WILL, TELOPT_BINARY,
    IAC, DO,   TELOPT_BINARY,
};

void telnet_ctx_send_init_cmds(TelnetCtx *ctx)
{
    telnet_write(ctx, telnet_init_cmds, sizeof(telnet_init_cmds));
}

ssize_t telnet_process(TelnetCtx *ctx, unsigned char *buf, ssize_t size)
{
    /* process buffer */
    if (size > 0) {
	unsigned char *buf2 = buf;
	ssize_t i = 0, i2 = 0;

	/* prescan. because IAC is rare, 
	 * this cost is worthy. */
	if (ctx->iac_state == IAC_NONE &&
	    !ctx->iac_quote &&
	    memchr(buf, IAC, size) == NULL)
	    return size;

	/* we have to look into the buffer. */
	for (i = 0; i < size; i++, buf++)
	    if(telnet_handler(ctx, *buf) == 0)
		*(buf2++) = *buf;
	    else
		i2 ++;
	size = (i2 == size) ? -1L : size - i2;
    }
    return size;
}

#ifdef DBG_OUTRPT
extern unsigned char fakeEscape;
#endif // DBG_OUTRPT

static void         
telnet_write  (TelnetCtx *ctx, const void *buf, size_t nbytes)
{
    if (ctx->callback->write_data)
	ctx->callback->write_data(ctx->write_arg, ctx->fd, buf, nbytes);
    else
	write(ctx->fd, buf, nbytes);
}

#ifndef _TELNET_DEFAULT_AYT_STRING
#define _TELNET_DEFAULT_AYT_STRING  "I'm still alive.\r\n"
#endif
static void         
telnet_send_ayt  (TelnetCtx *ctx)
{
    if (ctx->callback->send_ayt)
    {
	ctx->callback->send_ayt(ctx->ayt_arg, ctx->fd);
    }
    else
    {
	/* respond as fast as we can */
	telnet_write(ctx, _TELNET_DEFAULT_AYT_STRING, sizeof(_TELNET_DEFAULT_AYT_STRING)-1);
    }
}

/* input:  raw character
 * output: telnet command if c was handled, otherwise zero.
 */
static unsigned int 
telnet_handler(TelnetCtx *ctx, unsigned char c) 
{
    /* we have to quote all IACs. */
    if(c == IAC && !ctx->iac_quote) {
	ctx->iac_quote = 1;
	return NOP;
    }

    /* hash client telnet sequences */
    if (ctx->cc_arg && ctx->callback->update_client_code) {
	void (*cb)(void *, unsigned char) = 
	    ctx->callback->update_client_code;
	if(ctx->iac_state == IAC_WAIT_SE) {
	    // skip suboption
	} else {
	    if(ctx->iac_quote)
		cb(ctx->cc_arg, IAC);
	    cb(ctx->cc_arg, c);
	}
    }

    /* a special case is the top level iac. otherwise, iac is just a quote. */
    if (ctx->iac_quote) {
	if(ctx->iac_state == IAC_NONE)
	    ctx->iac_state = IAC_COMMAND;
	if(ctx->iac_state == IAC_WAIT_SE && c == SE)
	    ctx->iac_state = IAC_PROCESS_OPT;
	ctx->iac_quote = 0;
    }

    /* now, let's process commands by state */
    switch(ctx->iac_state) {

	case IAC_NONE:
	    return 0;

	case IAC_COMMAND:
#if 0 // def DEBUG
	    {
		int cx = c; /* to make compiler happy */
		telnet_write(ctx, "-", 1);
		if(TELCMD_OK(cx))
		    telnet_write(ctx, TELCMD(c), strlen(TELCMD(c)));
		telnet_write(ctx, " ", 1);
	    }
#endif
	    ctx->iac_state = IAC_NONE; /* by default we restore state. */
	    switch(c) {
		case IAC:
		    // return 0;
		    // we don't want to allow IACs as input.
		    return 1;

		/* we don't want to process these. or maybe in future. */
		case BREAK:           /* break */
#ifdef DBG_OUTRPT
		    fakeEscape = !fakeEscape;
		    return NOP;
#endif

		case ABORT:           /* Abort process */
		case SUSP:            /* Suspend process */
		case AO:              /* abort output--but let prog finish */
		case IP:              /* interrupt process--permanently */
		case EOR:             /* end of record (transparent mode) */
		case DM:              /* data mark--for connect. cleaning */
		case xEOF:            /* End of file: EOF is already used... */
		    return NOP;

		case NOP:             /* nop */
		    return NOP;

		/* we should process these, but maybe in future. */
		case GA:              /* you may reverse the line */
		case EL:              /* erase the current line */
		case EC:              /* erase the current character */
		    return NOP;

		/* good */
		case AYT:             /* are you there */
		    telnet_send_ayt(ctx);
		    return NOP;

		case DONT:            /* you are not to use option */
		case DO:              /* please, you use option */
		case WONT:            /* I won't use option */
		case WILL:            /* I will use option */
		    ctx->iac_opt_req = c;
		    ctx->iac_state = IAC_WAIT_OPT;
		    return NOP;

		case SB:              /* interpret as subnegotiation */
		    ctx->iac_state = IAC_WAIT_SE;
		    ctx->iac_buflen = 0;
		    return NOP;

		case SE:              /* end sub negotiation */
		default:
		    return NOP;
	    }
	    return 1;

	case IAC_WAIT_OPT:
#if 0 // def DEBUG
	    telnet_write(ctx, "-", 1);
	    if(TELOPT_OK(c))
		telnet_write(ctx, TELOPT(c), strlen(TELOPT(c)));
	    telnet_write(ctx, " ", 1);
#endif
	    ctx->iac_state = IAC_NONE;
	    /*
	     * According to RFC, there're some tricky steps to prevent loop.
	     * However because we have a poor term which does not allow 
	     * most abilities, let's be a strong boss here.
	     *
	     * Although my old imeplementation worked, it's even better to follow this:
	     * http://www.tcpipguide.com/free/t_TelnetOptionsandOptionNegotiation-3.htm
	     */
	    switch(c) {
		/* i-dont-care: i don't care about what client is. 
		 * these should be claimed in init and
		 * client must follow me. */
		case TELOPT_TTYPE:	/* termtype or line. */
		case TELOPT_NAWS:       /* resize terminal */
		case TELOPT_SGA:	/* supress GA */
		case TELOPT_ECHO:       /* echo */
		case TELOPT_BINARY:	/* we are CJK. */
		    break;

		/* i-dont-agree: i don't understand/agree these.
		 * according to RFC, saying NO stopped further
		 * requests so there'll not be endless loop. */
		case TELOPT_RCP:         /* prepare to reconnect */
		default:
		    if (ctx->iac_opt_req == WILL || ctx->iac_opt_req == DO)
		    {
			/* unknown option, reply with won't */
			unsigned char cmd[3] = { IAC, DONT, 0 };
			if(ctx->iac_opt_req == DO) cmd[1] = WONT;
			cmd[2] = c;
			telnet_write(ctx, cmd, sizeof(cmd));
		    }
		    break;
	    }
	    return 1;

	case IAC_WAIT_SE:
	    ctx->iac_buf[ctx->iac_buflen++] = c;
	    /* no need to convert state because previous quoting will do. */
		    
	    // XXX BUG max length of TERMTYPE is 40 characters...
	    // (and one extra NUL for prefix 'IS')
	    if(ctx->iac_buflen == TELNET_IAC_MAXLEN) {
		/* may be broken protocol?
		 * whether finished or not, break for safety 
		 * or user may be frozen.
		 */
		ctx->iac_state = IAC_NONE;
		return 0;
	    }
	    return 1;

	case IAC_PROCESS_OPT:
	    ctx->iac_state = IAC_NONE;
#if 0 // def DEBUG
	    telnet_write(ctx, "-", 1);
	    if(TELOPT_OK(ctx->iac_buf[0]))
		telnet_write(ctx, TELOPT(ctx->iac_buf[0]), strlen(TELOPT(ctx->iac_buf[0])));
	    telnet_write(ctx, " ", 1);
#endif
	    switch(ctx->iac_buf[0]) {

		/* resize terminal */
		case TELOPT_NAWS:
		    {
			int w = (ctx->iac_buf[1] << 8) + (ctx->iac_buf[2]);
			int h = (ctx->iac_buf[3] << 8) + (ctx->iac_buf[4]);
			if (ctx->callback->term_resize)
			    ctx->callback->term_resize(ctx->resize_arg, w, h);
			if (ctx->cc_arg &&
				ctx->callback->update_client_code) {
			    void (*cb)(void *, unsigned char) = 
				ctx->callback->update_client_code;
			    cb(ctx->cc_arg, ctx->iac_buf[0]);

			    if(w==80 && h==24)
				cb(ctx->cc_arg, 1);
			    else if(w==80)
				cb(ctx->cc_arg, 2);
			    else if(h==24)
				cb(ctx->cc_arg, 3);
			    else
				cb(ctx->cc_arg, 4);
			    cb(ctx->cc_arg, IAC);
			    cb(ctx->cc_arg, SE);
			}
		    }
		    break;

		case TELOPT_TTYPE:
		    // XXX termtype is in ctx->iac_buf+2
		    // iac_buf: TTYPE=24 IS=0 NVT_ASCII ...
		    if (ctx->callback->ttype && ctx->iac_buflen > 2)
			ctx->callback->ttype(
				ctx->ttype_arg,
				(char*)ctx->iac_buf+2,
				ctx->iac_buflen-2);
		    // no break here, let's also update termtype to cc.
		default:
		    if (ctx->cc_arg &&
			    ctx->callback->update_client_code) {
			void (*cb)(void *, unsigned char) = 
			    ctx->callback->update_client_code;
			
			unsigned int i;
			for(i=0;i<ctx->iac_buflen;i++)
			    cb(ctx->cc_arg, ctx->iac_buf[i]);
			cb(ctx->cc_arg, IAC);
			cb(ctx->cc_arg, SE);
		    }
		    break;
	    }
	    return 1;
    }
    return 1; /* never reached */
}

// vim: sw=4 
