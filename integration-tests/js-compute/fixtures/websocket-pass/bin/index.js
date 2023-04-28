/* eslint-env serviceworker */

import { env } from 'fastly:env';
import { handoffWebsocket } from "fastly:experimental";
addEventListener("fetch", event => {
    try {
        const path = (new URL(event.request.url)).pathname
        console.log(`path: ${path}`)
        console.log(`FASTLY_SERVICE_VERSION: ${env('FASTLY_SERVICE_VERSION')}`)
        if (path === '/raw') {
            event.request.headers.set('Host', 'ws.postman-echo.com')
            console.log(event.request)
            for (let h of event.request.headers.entries()) console.log(h)
            handoffWebsocket('postman-echo');
            return;
        } else if (path === '/wsws') {
            console.log(event.request)
            for (let h of event.request.headers.entries()) console.log(h)
            handoffWebsocket('ws-ifelse-io');
            return;
        }
        console.log(event.request)
        for (let h of event.request.headers.entries()) console.log(h)
        event.respondWith(app(event))
    } catch (error) {
        console.error(error)
        event.respondWith(new Response(`The routeHandler threw an error: ${error.message}` + '\n' + error.stack, {status:500}));
    }
});

const routes = new Map();
/**
 * @param {FetchEvent} event
 * @returns {Response}
 */
async function app(event) {
    try {
        const path = (new URL(event.request.url)).pathname
        console.log(`path: ${path}`)
        console.log(`FASTLY_SERVICE_VERSION: ${env('FASTLY_SERVICE_VERSION')}`)
        if (routes.has(path)) {
            const routeHandler = routes.get(path)
            const result = await routeHandler(event.request)
            return result;
        }
        return new Response(`${path} endpoint does not exist`, {status:404})
    } catch (error) {
        console.error(error)
        return new Response(`The routeHandler threw an error: ${error.message}` + '\n' + error.stack, {status:500});
    }
}

routes.set('/', () => new Response('meow'))