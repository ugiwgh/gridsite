/*
   Copyright (c) 2002-6, Andrew McNab, University of Manchester
   All rights reserved.

   Redistribution and use in source and binary forms, with or
   without modification, are permitted provided that the following
   conditions are met:

     o Redistributions of source code must retain the above
       copyright notice, this list of conditions and the following
       disclaimer. 
     o Redistributions in binary form must reproduce the above
       copyright notice, this list of conditions and the following
       disclaimer in the documentation and/or other materials
       provided with the distribution. 

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
   ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
*/

/*

Build with:

gcc -lcurl -lssl -lcrypto -o grst-proxy-put grst-proxy-put.c libgridsite.a

http://www.gridpp.ac.uk/authz/gridsite/

*/

#ifndef VERSION
#define VERSION "0.0.0"
#endif

#define _GNU_SOURCE

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include <getopt.h>

#include <gridsite.h>

#include <stdsoap2.h>

#include "soapH.h"
#include "DelegationSoapBinding.nsmap"

#define HTPROXY_PUT		0
#define HTPROXY_RENEW		1
#define HTPROXY_DESTROY		2
#define HTPROXY_TIME		3
#define HTPROXY_UNIXTIME	4
#define HTPROXY_MAKE		5
#define HTPROXY_INFO		6

void printsyntax(char *argv0)
{
  char *p;

  p = rindex(argv0, '/');
  if (p != NULL) ++p;
  else           p = argv0;

  fprintf(stderr, "%s [options] URL\n"
          "(Version: %s)\n", p, VERSION);
}

int htproxy_logfunc(char *file, int line, int level, char *fmt, ...)
{
  char *mesg;
  va_list ap;

  va_start(ap, fmt);
  vasprintf(&mesg, fmt, ap);
  va_end(ap);

  fprintf(stderr, "%s(%d) %s\n", file, line, mesg);

  free(mesg);
  
  return 0;
}
  
int main(int argc, char *argv[])
{
  char  *delegation_id = "", *reqtxt, *certtxt, *valid = NULL, 
        *cert = NULL, *key = NULL, *capath = NULL, *keycert, timestr[81],
        *vomsdir = "/etc/grid-security/vomsdir",
        *executable, *keytxt, *proxychain, *ptr, *p;
  struct ns__putProxyResponse *unused = NULL;
  struct tm *finish_tm;
  int    option_index, c, noverify = 0, i, ret,
         method = HTPROXY_PUT, verbose = 0, fd, minutes;
  struct soap soap_get, soap_put;
  struct ns__getNewProxyReqResponse     getNewProxyReqResponse;
  struct ns__renewProxyReqResponse      renewProxyReqResponse;
  struct ns__destroyResponse            destroyResponse;
  struct ns__getTerminationTimeResponse getTerminationTimeResponse;
  FILE   *ifp, *ofp;
  STACK_OF(X509) *x509_certstack;
  X509   *x509_cert;
  BIO    *certmem;
  GRSTx509Chain *grst_chain = NULL;
  GRSTx509Cert  *grst_cert = NULL;
  long   ptrlen;
  struct stat statbuf;
  struct passwd *userpasswd; 
  struct option long_options[] = {      {"verbose",     0, 0, 'v'},
                                        {"cert",        1, 0, 0},
                                        {"key",         1, 0, 0},
                                        {"capath",      1, 0, 0},
                                        {"destroy",     0, 0, 0},
                                        {"time",        0, 0, 0},
                                        {"no-verify",   0, 0, 0},
                                        {"valid",       1, 0, 0},
                                        {"delegation-id",1, 0, 0},
                                        {"put",         0, 0, 0},
                                        {"renew",       0, 0, 0},
                                        {"unixtime",	0, 0, 0},
                                        {"make",	0, 0, 0},
                                        {"info",	0, 0, 0},
                                        {0, 0, 0, 0}  };

  if (argc == 1)
    {
      printsyntax(argv[0]);
      return 0;
    }

  while (1)
       {
         option_index = 0;
                                                                                
         c = getopt_long(argc, argv, "v", long_options, &option_index);

         if      (c == -1) break;
         else if (c == 0)
           {
             if      (option_index ==  1) cert            = optarg;
             else if (option_index ==  2) key             = optarg;
             else if (option_index ==  3) capath          = optarg;
             else if (option_index ==  4) method          = HTPROXY_DESTROY;
             else if (option_index ==  5) method          = HTPROXY_TIME;
             else if (option_index ==  6) noverify        = 1;
             else if (option_index ==  7) valid           = optarg;
             else if (option_index ==  8) delegation_id   = optarg;
             else if (option_index ==  9) method          = HTPROXY_PUT;
             else if (option_index == 10) method          = HTPROXY_RENEW;
             else if (option_index == 11) method          = HTPROXY_UNIXTIME;
             else if (option_index == 12) method          = HTPROXY_MAKE;
             else if (option_index == 13) method          = HTPROXY_INFO;
           }
         else if (c == 'v') 
                {
                  GRSTerrorLogFunc = htproxy_logfunc;
                  ++verbose;
                }
       }

  executable = rindex(argv[0], '/');
  if (executable != NULL) executable++;
  else                    executable = argv[0];
  
  if    (strcmp(executable, "htproxydestroy") == 0) method = HTPROXY_DESTROY;
  else if (strcmp(executable, "htproxyrenew") == 0) method = HTPROXY_RENEW;
  else if (strcmp(executable, "htproxytime") == 0)  method = HTPROXY_TIME;
  else if (strcmp(executable, "htproxyunixtime") == 0) 
                                                    method = HTPROXY_UNIXTIME;
  else if (strcmp(executable, "htproxymake") == 0)  method = HTPROXY_MAKE;
  else if (strcmp(executable, "htproxyinfo") == 0)  method = HTPROXY_INFO;

  if ((method != HTPROXY_MAKE) && 
      (method != HTPROXY_INFO) && (optind + 1 != argc))
    {
      fprintf(stderr, "Must specify a delegation service URL!\n");
      return 1;
    }

  if ((method == HTPROXY_RENEW) && (delegation_id[0] == '\0'))
    {
      fprintf(stderr, "Must give a Delegation ID when renewing\n");
      return 1;
    }
    
  if (valid == NULL) minutes = 60 * 12;
  else minutes = atoi(valid);
  
  if (verbose) fprintf(stderr, "Proxy valid for %d minutes\n", minutes);
 
  ERR_load_crypto_strings ();
  OpenSSL_add_all_algorithms();

  if      ((cert == NULL) && (key != NULL)) cert = key;
  else if ((cert != NULL) && (key == NULL)) key = cert;
  else if ((cert == NULL) && (key == NULL))
    {
      if (method != HTPROXY_MAKE) cert = getenv("X509_USER_PROXY");

      if (cert != NULL) key = cert;
      else
        {
          if (method != HTPROXY_MAKE) 
               asprintf(&(cert), "/tmp/x509up_u%d", geteuid());

          /* one fine day, we will check the proxy file for 
             expiry too to avoid suprises when we try to use it ... */

          if (stat(cert, &statbuf) == 0) key = cert;
          else if (method != HTPROXY_INFO)
            {
              cert = getenv("X509_USER_CERT");
              key  = getenv("X509_USER_KEY");
                                                                                
              userpasswd = getpwuid(geteuid());
                                                                                
              if ((cert == NULL) &&
                  (userpasswd != NULL) &&
                  (userpasswd->pw_dir != NULL))
                asprintf(&(cert), "%s/.globus/usercert.pem",
                                                    userpasswd->pw_dir);
                                                                                
              if ((key == NULL) &&
                  (userpasswd != NULL) &&
                  (userpasswd->pw_dir != NULL))
                asprintf(&(key), "%s/.globus/userkey.pem",
                                                    userpasswd->pw_dir);
                                                                                
            }
        }
    }
                                                                                
  if (capath == NULL) capath = getenv("X509_CERT_DIR");
  if (capath == NULL) capath = "/etc/grid-security/certificates";

  if (verbose) fprintf(stderr, "key=%s\ncert=%s\ncapath=%s\n",
                       key, cert, capath);

  if ((key != NULL) && (cert != NULL) &&
      (strcmp(key, cert) != 0)) /* we have to concatenate for gSOAP */
    {
      keycert = strdup("/tmp/.XXXXXX");
        
      fd = mkstemp(keycert);
      ofp = fdopen(fd, "w");
      if (!ofp)
      {
          fprintf(stderr, "Cannot open tmp file for the key\n");
          return 1;
      }
          
      ifp = fopen(key, "r");          
      {
          fprintf(stderr, "Cannot open the file with the key\n");
          return 1;
      }
      while ((c = fgetc(ifp)) != EOF) fputc(c, ofp);          
      fclose(ifp);
          
      ifp = fopen(cert, "r");          
      {
          fprintf(stderr, "Cannot open the file with the cert\n");
          return 1;
      }
      while ((c = fgetc(ifp)) != EOF) fputc(c, ofp);          
      fclose(ifp);
          
      fclose(ofp);       
          
      if (verbose) fprintf(stderr, "Created %s key/cert file\n", keycert);
    }
  else keycert = key;

  if ((method == HTPROXY_PUT) || (method == HTPROXY_RENEW))
    {
      if (verbose) 
        {
          fprintf(stderr, "Using SOAP delegation protocol\n");
          fprintf(stderr, "Delegation-ID: %s\n", delegation_id);
        }

      soap_init(&soap_get);
  
      if (soap_ssl_client_context(&soap_get,
                                  SOAP_SSL_DEFAULT,
                                  keycert, 
                                  "",
                                  NULL,
                                  capath,
                                  NULL))
        {
          soap_print_fault(&soap_get, stderr);
          return 1;
        } 

      if ((method == HTPROXY_RENEW) && (delegation_id[0] != '\0'))
        {
          if (verbose) fprintf(stderr, "Send renewProxyReq to service\n");

          soap_call_ns__renewProxyReq(&soap_get, 
                                argv[optind],	/* HTTPS url of service */
                                "http://www.gridsite.org/namespaces/delegation-1",
                                delegation_id, 
                                &renewProxyReqResponse);
      
          if (soap_get.error)
            {
              soap_print_fault(&soap_get, stderr);
              return 1;        
            }
       
          reqtxt = renewProxyReqResponse._renewProxyReqReturn;
        }
      else
        {
          if (verbose) fprintf(stderr, "Send getNewProxyReq to service\n");

          soap_call_ns__getNewProxyReq(&soap_get,
                            argv[optind],	/* HTTPS url of service */
                            "http://www.gridsite.org/namespaces/delegation-1",
                            &getNewProxyReqResponse);

          if (soap_get.error)
            {
              soap_print_fault(&soap_get, stderr);
              return 1;        
            }

          if (!getNewProxyReqResponse.getNewProxyReqReturn)
            {
              fprintf(stderr, "Empty response from getNewProxyReq\n");
              return 1;
            }

          reqtxt = getNewProxyReqResponse.getNewProxyReqReturn->proxyRequest;
          delegation_id = 
                   getNewProxyReqResponse.getNewProxyReqReturn->delegationID;
        }
 
      if (verbose) fprintf(stderr, "reqtxt:\n%s", reqtxt);
      
      if (GRSTx509MakeProxyCert(&certtxt, stderr, reqtxt, cert, key, minutes) 
          != GRST_RET_OK)
        {
          return 1;
        }

      soap_init(&soap_put);
  
      if (verbose) fprintf(stderr, "Send putProxy to service:\n%s\n", certtxt);

      if (soap_ssl_client_context(&soap_put,
                                  SOAP_SSL_DEFAULT,
                                  keycert, 
                                  "",
                                  NULL,
                                  capath,
                                  NULL))
        {
          soap_print_fault(&soap_put, stderr);
          return 1;
        } 

      soap_call_ns__putProxy(&soap_put, argv[optind],
                             "http://www.gridsite.org/namespaces/delegation-1",
                             delegation_id, 
                             certtxt, unused);      
      if (soap_put.error)
        {
          soap_print_fault(&soap_put, stderr);
          return 1;        
        }

      puts(delegation_id);

      return 0;
    }  
  else if (method == HTPROXY_DESTROY)
    {
      if (verbose) 
        {
          fprintf(stderr, "Using SOAP proxy destroy protocol\n");
          fprintf(stderr, "Delegation-ID: %s\n", delegation_id);
        }

      soap_init(&soap_put);
  
      if (verbose) fprintf(stderr, "Send destroy to service:\n");

      if (soap_ssl_client_context(&soap_put,
                                  SOAP_SSL_DEFAULT,
                                  keycert, 
                                  "",
                                  NULL,
                                  capath,
                                  NULL))
        {
          soap_print_fault(&soap_put, stderr);
          return 1;
        } 

      soap_call_ns__destroy(&soap_put, argv[optind],
                             "http://www.gridsite.org/namespaces/delegation-1",
                             delegation_id, 
                             &destroyResponse);
      if (soap_put.error)
        {
          soap_print_fault(&soap_put, stderr);
          return 1;        
        }

      return 0;
    }  
  else if ((method == HTPROXY_TIME) || (method == HTPROXY_UNIXTIME))
    {
      if (verbose) 
        {
          fprintf(stderr, "Using SOAP proxy get expiration time protocol\n");
          fprintf(stderr, "Delegation-ID: %s\n", delegation_id);
        }

      soap_init(&soap_put);
  
      if (verbose) fprintf(stderr, "Send get time to service:\n");

      if (soap_ssl_client_context(&soap_put,
                                  SOAP_SSL_DEFAULT,
                                  keycert, 
                                  "",
                                  NULL,
                                  capath,
                                  NULL))
        {
          soap_print_fault(&soap_put, stderr);
          return 1;
        } 

      soap_call_ns__getTerminationTime(&soap_put, argv[optind],
                             "http://www.gridsite.org/namespaces/delegation-1",
                             delegation_id, 
                             &getTerminationTimeResponse);
      if (soap_put.error)
        {
          soap_print_fault(&soap_put, stderr);
          return 1;        
        }


      if (method == HTPROXY_UNIXTIME)
       printf("%ld\n", getTerminationTimeResponse._getTerminationTimeReturn);
      else
        {
          finish_tm = 
           localtime(&(getTerminationTimeResponse._getTerminationTimeReturn));

          strftime(timestr, sizeof(timestr),
                       "%a %b %e %H:%M:%S %Z %Y\n", finish_tm);
                       
          fputs(timestr, stdout);
        }
        
      return 0;
    }  
  else if (method == HTPROXY_MAKE)
#ifdef HT_LEAK_TEST
    {
    int ii;
    FILE *ffpp;
    char lineline[80];
    for (ii=0; ii < 1000; ++ii)
#endif
    {
      if (GRSTx509CreateProxyRequestKS(&reqtxt, &keytxt, NULL, 0) != GRST_RET_OK)
        {
          fprintf(stderr, "Failed to create internal proxy cert request\n");
          return 1;
        }
      
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d a %s", ii, lineline);
     fclose(ffpp);
#endif
      if (GRSTx509MakeProxyCert(&proxychain, NULL, reqtxt, cert, key, minutes)
            != GRST_RET_OK)
        {
          fprintf(stderr, "Failed to sign internal proxy cert request\n");
          return 2;
        }
        
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d b %s", ii, lineline);
     fclose(ffpp);
#endif

      /* convert back to cert stack so can output in the right order */
      if (GRSTx509StringToChain(&x509_certstack, proxychain) != GRST_RET_OK)
        {
          fprintf(stderr, "Failed to convert internal proxy chain\n");
          return 3;
        }
        
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d c %s", ii, lineline);
     fclose(ffpp);
#endif

      /* just the proxy certificate we have created */
      if ((x509_cert = sk_X509_value(x509_certstack, 0)))
        {
          certmem = BIO_new(BIO_s_mem());
          if (PEM_write_bio_X509(certmem, x509_cert) == 1)
            {
              ptrlen = BIO_get_mem_data(certmem, &ptr);
              fwrite(ptr, 1, ptrlen, stdout);
            }
                                                          
          BIO_free(certmem);
        }
                                                                    
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d d %s", ii, lineline);
     fclose(ffpp);
#endif
      /* then the private key */ 
      fputs(keytxt, stdout);
      
      /* and only now the rest of the certificates */
      for (i=1; i <= sk_X509_num(x509_certstack) - 1; ++i)
        /* loop through the proxy chain starting at 2nd most recent proxy */
         {
           if ((x509_cert = sk_X509_value(x509_certstack, i)))
             {
               certmem = BIO_new(BIO_s_mem());
               if (PEM_write_bio_X509(certmem, x509_cert) == 1)
                 {
                   ptrlen = BIO_get_mem_data(certmem, &ptr);
                   fwrite(ptr, 1, ptrlen, stdout);
                 }

               BIO_free(certmem);
             }
         }

 
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d e %s", ii, lineline);
     fclose(ffpp);
#endif
      free(proxychain);
      free(keytxt);
      free(reqtxt);
      sk_X509_free(x509_certstack);
      
#ifdef HT_LEAK_TEST      
     ffpp = fopen("/proc/self/statm", "r");
     fgets(lineline, sizeof(lineline), ffpp);
     fprintf(stderr, "%d f %s", ii, lineline);
     fclose(ffpp);
    }
#endif
      return 0;
    }
  else if (method == HTPROXY_INFO)
    {
      if (cert != NULL) 
        {
          if (verbose) fprintf(stderr, "Getting proxy info from %s\n", cert);
    
          ifp = fopen(cert, "r");
          if (ifp == NULL)
            {
              fprintf(stderr, "Failed to open proxy file\n");
              return 2;              
            }
        }
      else  
        {
          if (verbose) fprintf(stderr, "Getting proxy info from stdin\n");
          ifp = stdin;
        }
      
      ptrlen = 4096;
      ptr = malloc(ptrlen);
      i = 0;
      
      while ((c = fgetc(ifp)) != EOF)
           {
             ptr[i] = c;
             ++i;
             
             if (i >= ptrlen) 
               {
                 ptrlen += 4096;
                 ptr = realloc(ptr, ptrlen);
               }
           }
           
      ptr[i] = '\0';
      if (cert != NULL) fclose(ifp);
      
      if ((GRSTx509StringToChain(&x509_certstack, ptr) != GRST_RET_OK) ||
          (x509_certstack == NULL))
        {
          fprintf(stderr, "Failed to parse proxy file for certificate chain\n");
          free(ptr);
          return 2;
        }

      free(ptr);

      if (verbose) fprintf(stderr, "Parsing certificate chain\n");
      
      ret = GRSTx509ChainLoadCheck(&grst_chain, x509_certstack, NULL,  
                                   capath, vomsdir);
      
      if ((ret != GRST_RET_OK) || 
          (grst_chain == NULL) || (grst_chain->firstcert == NULL))
        {
          fprintf(stderr, "Failed parsing certificate chain\n");
          return 3;
        }
      
      grst_cert = grst_chain->firstcert;

      for (i=0; grst_cert != NULL; grst_cert = grst_cert->next, ++i)
         {
           if      (grst_cert->type == GRST_CERT_TYPE_CA)    p = "(CA) ";
           else if (grst_cert->type == GRST_CERT_TYPE_EEC)   p = "(EEC) ";
           else if (grst_cert->type == GRST_CERT_TYPE_PROXY) p = "(PC) ";
           else if (grst_cert->type == GRST_CERT_TYPE_VOMS)  p = "(AC) ";
           else p = "";
                              
           printf("%d %s%s\n", i, p,
                  (grst_cert->type == GRST_CERT_TYPE_VOMS) 
                    ? grst_cert->value : grst_cert->dn);
 
           printf(" Status     : %d ( %s%s%s%s%s%s)\n", grst_cert->errors,
                 (grst_cert->errors == 0) ? "OK " : "",
                 (grst_cert->errors & GRST_CERT_BAD_FORMAT) ? "BAD_FORMAT ":"",
                 (grst_cert->errors & GRST_CERT_BAD_CHAIN)  ? "BAD_CHAIN ":"",
                 (grst_cert->errors & GRST_CERT_BAD_SIG)    ? "BAD_SIG ":"",
                 (grst_cert->errors & GRST_CERT_BAD_TIME)   ? "BAD_TIME ":"",
                 (grst_cert->errors & GRST_CERT_BAD_OCSP)   ? "BAD_OCSP ":"");

           printf(" Start      : %s",   ctime(&(grst_cert->notbefore)));
           printf(" Finish     : %s",   ctime(&(grst_cert->notafter)));
           printf(" Delegation : %d\n", grst_cert->delegation);

           if (grst_cert->type == GRST_CERT_TYPE_VOMS)
             {
               printf(" User DN    : %s\n", grst_cert->dn);
               printf(" VOMS DN    : %s\n\n", grst_cert->issuer);
             }
           else
             {
               printf(" Serial     : %s\n", grst_cert->serial);
               printf(" Issuer     : %s\n\n", grst_cert->issuer);              
             }
         }
      
      GRSTx509ChainFree(grst_chain);
    }
  /* weirdness */
}

